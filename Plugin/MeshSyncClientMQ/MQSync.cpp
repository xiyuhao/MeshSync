#include "pch.h"
#include "MQSync.h"
#include "MQUtils.h"


MQSync::MQSync(MQBasePlugin *plugin)
{
    m_plugin = plugin;
}

MQSync::~MQSync()
{
    try {
        if (m_future_meshes.valid()) {
            m_future_meshes.get();
        }
        if (m_future_camera.valid()) {
            m_future_camera.get();
        }
    }
    catch (...) {
    }
}

ms::ClientSettings& MQSync::getClientSettings() { return m_settings; }
std::string& MQSync::getCameraPath() { return m_host_camera_path; }
float& MQSync::getScaleFactor() { return m_scale_factor; }
bool& MQSync::getAutoSync() { return m_auto_sync; }
bool& MQSync::getSyncNormals() { return m_sync_normals; }
bool & MQSync::getSyncVertexColor() { return m_sync_vertex_color; }
bool& MQSync::getSyncCamera() { return m_sync_camera; }
bool& MQSync::getSyncBones() { return m_sync_bones; }
bool& MQSync::getSyncPoses() { return m_sync_poses; }
bool& MQSync::getSyncTextures() { return m_sync_textures; }

bool& MQSync::getBakeSkin() { return m_bake_skin; }
bool& MQSync::getBakeCloth() { return m_bake_cloth; }

void MQSync::clear()
{
    m_meshes.clear();
    m_bones.clear();
    m_client_meshes.clear();
    m_host_meshes.clear();

    m_texture_manager.clear();
    m_materials.clear();
    m_camera.reset();
    m_mesh_exists.clear();
    m_bone_exists.clear();
    m_pending_send_meshes = false;
}

void MQSync::flushPendingRequests(MQDocument doc)
{
    if (m_pending_send_meshes) {
        sendMeshes(doc, true);
    }
}

void MQSync::sendMeshes(MQDocument doc, bool force)
{
    if (!force)
    {
        if (!m_auto_sync) { return; }
    }

    // just return if previous request is in progress. responsiveness is highest priority.
    if (m_future_meshes.valid() &&
        m_future_meshes.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout)
    {
        m_pending_send_meshes = true;
        return;
    }

    m_pending_send_meshes = false;

    {
        int nobj = doc->GetObjectCount();
        int num_mesh_data = 0;

        // build relations
        for (int i = 0; i < nobj; ++i) {
            auto obj = doc->GetObject(i);
            if (!obj) { continue; }

            char name[MaxNameBuffer];
            obj->GetName(name, sizeof(name));

            MeshData rel;
            rel.obj = obj;
            m_meshes.push_back(std::move(rel));
            ++num_mesh_data;
        }

        {
            while ((int)m_client_meshes.size() < num_mesh_data) {
                m_client_meshes.push_back(ms::Mesh::create());
            }

            int mi = 0;
            for (size_t i = 0; i < m_meshes.size(); ++i) {
                auto& rel = m_meshes[i];
                rel.data = m_client_meshes[mi++];
                rel.data->index = (int)i;
            }
        }

        // gather mesh data
        parallel_for_each(m_meshes.begin(), m_meshes.end(), [this, doc](MeshData& rel) {
            rel.data->clear();
            rel.data->path = ms::ToUTF8(BuildPath(doc, rel.obj).c_str());
            ExtractID(rel.data->path.c_str(), rel.data->id);

            bool visible = rel.obj->GetVisible() != 0;
            rel.data->visible = visible;
            if (!visible) {
                // not send actual contents if not visible
                return;
            }

            extractMeshData(doc, rel.obj, *rel.data);
            m_mesh_manager.add(rel.data);
        });
    }

    {
        // gather material data
        int nmat = doc->GetMaterialCount();
        m_materials.reserve(nmat);
        for (int mi = 0; mi < nmat; ++mi) {
            if (auto src = doc->GetMaterial(mi)) {
                auto dst = ms::Material::create();
                m_materials.push_back(dst);

                char buf[1024];
                dst->id = mi;
                {
                    src->GetName(buf, sizeof(buf));
                    dst->name = ms::ToUTF8(buf);
                }
                dst->setColor(to_float4(src->GetColor()));
                if (m_sync_textures) {
                    src->GetTextureName(buf, sizeof(buf));
                    dst->setColorMap(exportTexture(buf, ms::TextureType::Default));
                }
            }
        }
    }

#if MQPLUGIN_VERSION >= 0x0464
    if (m_sync_bones) {
        // gather bone data
        MQBoneManager bone_manager(m_plugin, doc);

        int nbones = bone_manager.GetBoneNum();
        if (nbones == 0) {
            goto bone_end;
        }

        {
            std::wstring name;
            std::vector<UINT> bone_ids;

            bone_manager.EnumBoneID(bone_ids);
            for (auto bid : bone_ids) {
                auto& bone = m_bones[bid];
                bone.id = bid;

                bone_manager.GetName(bid, name);
                bone.name = S(name);

                UINT parent;
                bone_manager.GetParent(bid, parent);
                bone.parent = parent;

                MQPoint base_pos;
                bone_manager.GetBaseRootPos(bid, base_pos);
                bone.world_pos = (const float3&)base_pos;
                bone.bindpose = mu::invert(mu::transform(bone.world_pos, quatf::identity(), float3::one()));

                if (m_sync_poses) {
                    MQMatrix rot;
                    bone_manager.GetRotationMatrix(bid, rot);
                    bone.world_rot = mu::invert(mu::to_quat((float4x4&)rot));
                }
                else {
                    bone.world_rot = quatf::identity();
                }
            }

            for (auto& pair : m_bones) {
                auto& bone = pair.second;

                // build path
                auto& path = bone.transform->path;
                path = "/bones";
                buildBonePath(path, bone);

                // setup transform
                {
                    auto& trans = *bone.transform;
                    trans.rotation = bone.world_rot;
                    trans.position = bone.world_pos;
                    auto it = m_bones.find(bone.parent);
                    if (it != m_bones.end())
                        trans.position -= it->second.world_pos;
                }
            }

            // get weights
            parallel_for_each(m_meshes.begin(), m_meshes.end(), [this, &bone_manager, nbones, &bone_ids](MeshData& rel) {
                auto obj = rel.obj;
                if (!rel.data->visible) { return; }

                std::vector<UINT> vertex_ids;
                std::vector<float> weights;
                for (auto bid : bone_ids) {
                    int nweights = bone_manager.GetWeightedVertexArray(bid, obj, vertex_ids, weights);
                    if (nweights == 0)
                        continue;

                    // find root bone
                    if (rel.data->root_bone.empty()) {
                        auto it = m_bones.find(bid);
                        for (;;) {
                            auto next = m_bones.find(it->second.parent);
                            if (next == m_bones.end())
                                break;
                            it = next;
                        }
                        rel.data->root_bone = it->second.transform->path;
                    }

                    auto data = ms::BoneData::create();
                    rel.data->bones.push_back(data);
                    rel.data->flags.has_bones = 1;
                    auto& bone = m_bones[bid];
                    data->path = bone.transform->path;
                    data->bindpose = bone.bindpose;

                    auto size = rel.data->points.size();
                    data->weights.resize_zeroclear(size);
                    for (int iw = 0; iw < nweights; ++iw) {
                        int vi = obj->GetVertexIndexFromUniqueID(vertex_ids[iw]);
                        if (vi >= 0) {
                            data->weights[vi] = weights[iw];
                        }
                    }
                }
            });
        }
    bone_end:;
    }
#endif


    // kick async send
    m_future_meshes = std::async(std::launch::async, [this]() {
        ms::Client client(m_settings);

        ms::SceneSettings scene_settings;
        scene_settings.handedness = ms::Handedness::Right;
        scene_settings.scale_factor = m_scale_factor;

        // notify scene begin
        {
            ms::FenceMessage fence;
            fence.type = ms::FenceMessage::FenceType::SceneBegin;
            client.send(fence);
        }

        // send materials and bones
        {
            ms::SetMessage set;
            set.scene.settings = scene_settings;
            set.scene.textures = m_texture_manager.getDirtyTextures();
            set.scene.materials = m_materials;
            for (auto& pair : m_bones) {
                set.scene.objects.push_back(pair.second.transform);
            }
            {
                auto objs = m_mesh_manager.getDirtyTransforms();
                set.scene.objects.insert(set.scene.objects.end(), objs.begin(), objs.end());
            }
            client.send(set);

            m_texture_manager.clear();
            m_materials.clear();
            m_bones.clear();
        }

        // send meshes one by one to Unity can respond quickly
        {
            auto meshes = m_mesh_manager.getDirtyMeshes();
            for (auto& mesh : meshes) {
                ms::SetMessage set;
                set.scene.settings = scene_settings;
                set.scene.objects = { mesh };
                client.send(set);
            };
            m_meshes.clear();
            m_mesh_manager.clearDirtyFlags();
        }

        // detect deleted objects and send delete message
        {
            for (auto& e : m_mesh_exists)
                e.second = false;
            for (auto& rel : m_meshes) {
                auto& path = rel.data->path;
                if (!path.empty())
                    m_mesh_exists[path] = true;
            }
            for (auto& e : m_bone_exists)
                e.second = false;
            for (auto& rel : m_bones) {
                auto& path = rel.second.transform->path;
                if (!path.empty())
                    m_bone_exists[path] = true;
            }

            ms::DeleteMessage del;
            for (auto i = m_mesh_exists.begin(); i != m_mesh_exists.end(); ) {
                if (!i->second) {
                    int id = 0;
                    ExtractID(i->first.c_str(), id);
                    del.targets.push_back({ i->first , id });
                    m_mesh_exists.erase(i++);
                }
                else
                    ++i;
            }
            for (auto i = m_bone_exists.begin(); i != m_bone_exists.end(); ) {
                if (!i->second) {
                    int id = 0;
                    ExtractID(i->first.c_str(), id);
                    del.targets.push_back({ i->first , id });
                    m_bone_exists.erase(i++);
                }
                else
                    ++i;
            }
            if (!del.targets.empty()) {
                client.send(del);
            }
        }

        // notify scene end
        {
            ms::FenceMessage fence;
            fence.type = ms::FenceMessage::FenceType::SceneEnd;
            client.send(fence);
        }
    });
}

void MQSync::buildBonePath(std::string& dst, BoneData& bd)
{
    auto it = m_bones.find(bd.parent);
    if (it != m_bones.end())
        buildBonePath(dst, it->second);

    dst += "/";
    dst += bd.name;
}


void MQSync::sendCamera(MQDocument doc, bool force)
{
    if (!force)
    {
        if (!m_auto_sync) { return; }
    }
    if (!m_sync_camera) { return; }

    // just return if previous request is in progress
    if (m_future_camera.valid() &&
        m_future_camera.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout)
    {
        return;
    }

    // gather camera data
    if (auto scene = doc->GetScene(0)) { // GetScene(0): perspective view
        if (!m_camera) {
            m_camera = ms::Camera::create();
            m_camera->near_plane *= m_scale_factor;
            m_camera->far_plane *= m_scale_factor;
        }
        auto prev_pos = m_camera->position;
        auto prev_rot = m_camera->rotation;
        auto prev_fov = m_camera->fov;

        m_camera->path = m_host_camera_path;
        extractCameraData(doc, scene, *m_camera);

        if (!force &&
            m_camera->position == prev_pos &&
            m_camera->rotation == prev_rot &&
            m_camera->fov == prev_fov)
        {
            // no need to send
            return;
        }
    }

    // kick async send
    m_future_camera = std::async(std::launch::async, [this]() {
        ms::Client client(m_settings);

        ms::SceneSettings scene_settings;
        scene_settings.handedness = ms::Handedness::Right;
        scene_settings.scale_factor = m_scale_factor;

        // notify scene begin
        {
            ms::FenceMessage fence;
            fence.type = ms::FenceMessage::FenceType::SceneBegin;
            client.send(fence);
        }

        // send camera
        {
            ms::SetMessage set;
            set.scene.settings = scene_settings;
            set.scene.objects = { m_camera };
            client.send(set);
        }

        // notify scene end
        {
            ms::FenceMessage fence;
            fence.type = ms::FenceMessage::FenceType::SceneEnd;
            client.send(fence);
        }
    });
}


bool MQSync::importMeshes(MQDocument doc)
{
    ms::Client client(m_settings);
    ms::GetMessage gd;
    gd.flags.get_transform = 1;
    gd.flags.get_indices = 1;
    gd.flags.get_points = 1;
    gd.flags.get_uv0 = 1;
    gd.flags.get_colors = 1;
    gd.flags.get_material_ids = 1;
    gd.scene_settings.handedness = ms::Handedness::Right;
    gd.scene_settings.scale_factor = m_scale_factor;
    gd.refine_settings.flags.apply_local2world = 1;
    gd.refine_settings.flags.invert_v = 1;
    gd.refine_settings.flags.bake_skin = m_bake_skin;
    gd.refine_settings.flags.bake_cloth = m_bake_cloth;

    auto ret = client.send(gd);
    if (!ret) {
        return false;
    }

    // import materials
    {
        // create unique name list as Metasequoia doesn't accept multiple same names
        std::vector<std::string> names;
        names.reserve(ret->materials.size());
        for (int i = 0; i < (int)ret->materials.size(); ++i) {
            auto name = ret->materials[i]->name;
            while (std::find(names.begin(), names.end(), name) != names.end()) {
                name += '_';
            }
            names.push_back(name);
        }

        while (doc->GetMaterialCount() < (int)ret->materials.size()) {
            doc->AddMaterial(MQ_CreateMaterial());
        }
        for (int i = 0; i < (int)ret->materials.size(); ++i) {
            auto dst = doc->GetMaterial(i);
            dst->SetName(ms::ToANSI(names[i]).c_str());
            dst->SetColor(to_MQColor(ret->materials[i]->getColor()));
        }
    }
    
    // import meshes
    for (auto& data : ret->objects) {
        if (data->getType() == ms::Entity::Type::Mesh) {
            auto& mdata = (ms::Mesh&)*data;

            // create name that includes ID
            char name[MaxNameBuffer];
            sprintf(name, "%s [id:%08x]", ms::ToANSI(mdata.getName()).c_str(), mdata.id);

            if (auto obj = findMesh(doc, name)) {
                doc->DeleteObject(doc->GetObjectIndex(obj));
            }
            auto obj = createMesh(doc, mdata, name);
            doc->AddObject(obj);

            m_host_meshes[mdata.id] = std::static_pointer_cast<ms::Mesh>(data);
        }
    }
    return true;
}

int MQSync::exportTexture(const std::string& path, ms::TextureType type)
{
    return m_texture_manager.addFile(path, type);
}

MQObject MQSync::findMesh(MQDocument doc, const char *name)
{
    int nobj = doc->GetObjectCount();
    for (int i = 0; i < nobj; ++i) {
        auto obj = doc->GetObject(i);
        if (!obj) { continue; }

        char tmp[MaxNameBuffer];
        obj->GetName(tmp, sizeof(tmp));
        if (strcmp(tmp, name) == 0) {
            return obj;
        }
    }
    return nullptr;
}

MQObject MQSync::createMesh(MQDocument doc, const ms::Mesh& data, const char *name)
{
    auto ret = MQ_CreateObject();

    ret->SetName(name);

    // gave up importing transform

    ret->SetSmoothAngle(data.refine_settings.smooth_angle);

    for (auto& p : data.points) {
        ret->AddVertex((MQPoint&)p);
    }
    {
        size_t nindices = data.indices.size();
        for (size_t i = 0; i < nindices; i += 3) {
            ret->AddFace(3, const_cast<int*>(&data.indices[i]));
        }
    }
    if(!data.uv0.empty()) {
        float2 uv[3];
        size_t nfaces = data.indices.size() / 3;
        for (size_t i = 0; i < nfaces; ++i) {
            uv[0] = data.uv0[data.indices[i * 3 + 0]];
            uv[1] = data.uv0[data.indices[i * 3 + 1]];
            uv[2] = data.uv0[data.indices[i * 3 + 2]];
            ret->SetFaceCoordinateArray((int)i, (MQCoordinate*)uv);
        }
    }
    if (!data.colors.empty()) {
        size_t nfaces = data.indices.size() / 3;
        for (size_t i = 0; i < nfaces; ++i) {
            ret->SetFaceVertexColor((int)i, 0, mu::Float4ToColor32(data.colors[data.indices[i * 3 + 0]]));
            ret->SetFaceVertexColor((int)i, 1, mu::Float4ToColor32(data.colors[data.indices[i * 3 + 1]]));
            ret->SetFaceVertexColor((int)i, 2, mu::Float4ToColor32(data.colors[data.indices[i * 3 + 2]]));
        }
        // enable vertex color flag on assigned materials
        auto mids = data.material_ids;
        mids.erase(std::unique(mids.begin(), mids.end()), mids.end());
        for (auto mid : mids) {
            if (mid >= 0) {
                doc->GetMaterial(mid)->SetVertexColor(MQMATERIAL_VERTEXCOLOR_DIFFUSE);
            }
        }
    }
    if (!data.material_ids.empty()) {
        size_t nfaces = data.indices.size() / 3;
        for (size_t i = 0; i < nfaces; ++i) {
            ret->SetFaceMaterial((int)i, data.material_ids[i]);
        }
    }
    return ret;
}

void MQSync::extractMeshData(MQDocument doc, MQObject obj, ms::Mesh& dst)
{
    dst.flags.has_points = 1;
    dst.flags.has_uv0 = 1;
    dst.flags.has_counts = 1;
    dst.flags.has_indices = 1;
    dst.flags.has_material_ids = 1;
    dst.flags.has_refine_settings = 1;

    dst.refine_settings.flags.gen_tangents = 1;
    dst.refine_settings.flags.invert_v = 1;
    if (obj->GetMirrorType() != MQOBJECT_MIRROR_NONE) {
        int axis = obj->GetMirrorAxis();
        dst.refine_settings.flags.mirror_x = (axis & MQOBJECT_MIRROR_AXIS_X) ? 1 : 0;
        dst.refine_settings.flags.mirror_y = (axis & MQOBJECT_MIRROR_AXIS_Y) ? 1 : 0;
        dst.refine_settings.flags.mirror_z = (axis & MQOBJECT_MIRROR_AXIS_Z) ? 1 : 0;
        if (obj->GetMirrorType() == MQOBJECT_MIRROR_JOIN) {
            dst.refine_settings.flags.mirror_x_weld = dst.refine_settings.flags.mirror_x;
            dst.refine_settings.flags.mirror_y_weld = dst.refine_settings.flags.mirror_y;
            dst.refine_settings.flags.mirror_z_weld = dst.refine_settings.flags.mirror_z;
        }
    }

    // transform
    {
        dst.refine_settings.flags.apply_world2local = 1;
        auto ite = m_host_meshes.find(dst.id);
        if (ite != m_host_meshes.end()) {
            dst.refine_settings.world2local = ite->second->refine_settings.world2local;
            dst.flags.apply_trs = 0;
        }
        else {
            dst.flags.apply_trs = 1;
            ExtractLocalTransform(obj, dst.position, dst.rotation, dst.scale);
            dst.refine_settings.world2local = invert(ExtractGlobalMatrix(doc, obj));
        }
    }

    // vertices
    int npoints = obj->GetVertexCount();
    dst.points.resize(npoints);
    obj->GetVertexArray((MQPoint*)dst.points.data());

    // faces
    int nfaces = obj->GetFaceCount();
    int nindices = 0;
    dst.counts.resize(nfaces);
    for (int fi = 0; fi < nfaces; ++fi) {
        int c = obj->GetFacePointCount(fi);
        dst.counts[fi] = c;
        nindices += c;
    }

    // indices, uv, material ID
    dst.indices.resize_discard(nindices);
    dst.uv0.resize_discard(nindices);
    dst.material_ids.resize_discard(nfaces);
    auto *indices = dst.indices.data();
    auto *uv = dst.uv0.data();
    for (int fi = 0; fi < nfaces; ++fi) {
        int c = dst.counts[fi];
        dst.material_ids[fi] = obj->GetFaceMaterial(fi);
        //if (obj->GetFaceVisible(fi))
        {
            obj->GetFacePointArray(fi, indices);
            obj->GetFaceCoordinateArray(fi, (MQCoordinate*)uv);
            indices += c;
            uv += c;
        }
    }

    // vertex colors
    if (m_sync_vertex_color) {
        dst.colors.resize_discard(nindices);
        dst.flags.has_colors = 1;
        auto *colors = dst.colors.data();
        for (int fi = 0; fi < nfaces; ++fi) {
            int count = dst.counts[fi];
            //if (obj->GetFaceVisible(fi))
            {
                for (int ci = 0; ci < count; ++ci) {
                    *(colors++) = Color32ToFloat4(obj->GetFaceVertexColor(fi, ci));
                }
            }
        }
    }

    // normals
#if MQPLUGIN_VERSION >= 0x0460
    if (m_sync_normals) {
        dst.normals.resize_discard(nindices);
        dst.flags.has_normals = 1;
        auto *normals = dst.normals.data();
        for (int fi = 0; fi < nfaces; ++fi) {
            int count = dst.counts[fi];
            BYTE flags;
            //if (obj->GetFaceVisible(fi))
            {
                for (int ci = 0; ci < count; ++ci) {
                    obj->GetFaceVertexNormal(fi, ci, flags, (MQPoint&)*(normals++));
                }
            }
        }
    }
    else
#endif
    {
        dst.refine_settings.flags.gen_normals_with_smooth_angle = 1;
        dst.refine_settings.smooth_angle = obj->GetSmoothAngle();
    }
}

void MQSync::extractCameraData(MQDocument doc, MQScene scene, ms::Camera& dst)
{
    dst.position = to_float3(scene->GetCameraPosition());
    auto eular = ToEular(scene->GetCameraAngle(), true);
    dst.rotation = rotateZXY(eular);

    dst.fov = scene->GetFOV() * mu::Rad2Deg;
#if MQPLUGIN_VERSION >= 0x450
    dst.near_plane = scene->GetFrontClip();
    dst.far_plane = scene->GetBackClip();
#endif
}
