/**********************************************************************
Copyright (c) 2018 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
********************************************************************/

#include "utils.h"
#include "render.h"
#include "scene_io.h"
#include "material_io.h"
#include "SceneGraph/light.h"
#include "Output/clwoutput.h"
#include "BaikalIO/image_io.h"

#include <filesystem>
#include <fstream>
#include "XML/tinyxml2.h"

using namespace Baikal;

namespace
{
    std::uint32_t constexpr kNumIterations = 4096;

    bool operator != (RadeonRays::float3 left, RadeonRays::float3 right)
    {
        return (left.x != right.x) ||
               (left.y != right.y) ||
               (left.z != right.z);
    }
}

struct OutputDesc
{
    Renderer::OutputType type;
    std::string name;
    // extension of the file to save
    std::string file_ext;
    // bites per pixel
    int width, height;
};

// if you need to add new output for saving to disk
// just put its description in thic collection
std::vector<OutputDesc> outputs_collection = {
    { Renderer::OutputType::kColor, "color", "png" },
    { Renderer::OutputType::kViewShadingNormal, "view_shading_depth", "png" },
    { Renderer::OutputType::kDepth, "depth", "png" },
    { Renderer::OutputType::kAlbedo, "albedo", "png" },
    { Renderer::OutputType::kGloss, "gloss", "png" }
};

Render::Render(std::string file_name,
               std::uint32_t output_width,
               std::uint32_t output_height)
{
    std::vector<CLWPlatform> platforms;
    CLWPlatform::CreateAllPlatforms(platforms);

    int platform_index = 0;

    for (auto j = 0u; j < platforms.size(); ++j)
    {
        for (auto i = 0u; i < platforms[j].GetDeviceCount(); ++i)
        {
            if (platforms[j].GetDevice(i).GetType() == CL_DEVICE_TYPE_GPU)
            {
                platform_index = j;
                break;
            }
        }
    }

    int device_index = 0;

    for (auto i = 0u; i < platforms[platform_index].GetDeviceCount(); ++i)
    {
        if (platforms[platform_index].GetDevice(i).GetType() == CL_DEVICE_TYPE_GPU)
        {
            device_index = i;
            break;
        }
    }

    auto platform = platforms[platform_index];
    auto device = platform.GetDevice(device_index);
    m_context = CLWContext::Create(device);

    m_factory = std::make_unique<Baikal::ClwRenderFactory>(m_context, "cache");
    m_renderer = m_factory->CreateRenderer(Baikal::ClwRenderFactory::RendererType::kUnidirectionalPathTracer);
    m_controller = m_factory->CreateSceneController();

    for (auto& output_info: outputs_collection)
    {
        m_outputs.push_back(m_factory->CreateOutput(output_width, output_height));
        m_renderer->SetOutput(output_info.type, m_outputs.back().get());
        output_info.width = output_width;
        output_info.height = output_height;
    }

    std::filesystem::path full_path = file_name;

    if (!full_path.has_filename())
    {
        THROW_EX("There is no any file to load");
    }
    if (!full_path.has_parent_path())
    {
        THROW_EX("can not read directory from input scene 'file_name'");
    }


    m_scene = Baikal::SceneIo::LoadScene(full_path.string(), full_path.parent_path().string());
}

void Render::LoadMaterialXml(const std::string &file_name)
{
    // Check it we have material remapping
    std::ifstream in_materials(file_name);
    std::ifstream in_mapping(file_name);

    if (in_materials && in_mapping)
    {
        in_materials.close();
        in_mapping.close();

        auto material_io = Baikal::MaterialIo::CreateMaterialIoXML();
        auto mats = material_io->LoadMaterials(file_name);
        auto mapping = material_io->LoadMaterialMapping(file_name);

        material_io->ReplaceSceneMaterials(*m_scene, *mats, mapping);
    }
}

void Render::LoadCameraXml(const std::string &file_name)
{
    tinyxml2::XMLDocument doc;
    doc.LoadFile(file_name.c_str());

    auto root = doc.FirstChildElement("cam_list");

    if (!root)
    {
        THROW_EX("Failed to open lights set file.")
    }

    tinyxml2::XMLElement* elem = root->FirstChildElement("camera");

    m_camera_states.clear();

    while (elem)
    {
        CameraInfo cam_info;

        // eye
        cam_info.pos.x = elem->FloatAttribute("cpx");
        cam_info.pos.y = elem->FloatAttribute("cpy");
        cam_info.pos.z = elem->FloatAttribute("cpz");

        // center
        cam_info.at.x = elem->FloatAttribute("tpx");
        cam_info.at.y = elem->FloatAttribute("tpy");
        cam_info.at.z = elem->FloatAttribute("tpz");

        // up
        cam_info.up.x = elem->FloatAttribute("upx");
        cam_info.up.y = elem->FloatAttribute("upy");
        cam_info.up.z = elem->FloatAttribute("upz");

        //other values
        cam_info.focal_length = elem->FloatAttribute("focal_length");
        cam_info.focus_distance = elem->FloatAttribute("focus_dist");
        cam_info.aperture = elem->FloatAttribute("aperture");

        m_camera_states.push_back(cam_info);
        elem = elem->NextSiblingElement("camera");
    }
}

void Render::LoadLightXml(const std::string &file_name)
{
    tinyxml2::XMLDocument doc;
    doc.LoadFile(file_name.c_str());
    auto root = doc.FirstChildElement("light_list");

    if (!root)
    {
        THROW_EX("Failed to open lights set file.")
    }

    Light::Ptr new_light;
    tinyxml2::XMLElement* elem = root->FirstChildElement("light");

    while (elem)
    {
        //type
        std::string type = elem->Attribute("type");
        if (type == "point")
        {
            new_light = PointLight::Create();
        }
        else if (type == "direct")
        {
            new_light = DirectionalLight::Create();
        }
        else if (type == "spot")
        {
            new_light = SpotLight::Create();
            RadeonRays::float2 cs;
            cs.x = elem->FloatAttribute("csx");
            cs.y = elem->FloatAttribute("csy");
            //this option available only for spot light
            SpotLight::Ptr spot = std::dynamic_pointer_cast<SpotLight>(new_light);
            spot->SetConeShape(cs);
        }
        else if (type == "ibl")
        {
            new_light = ImageBasedLight::Create();
            std::string tex_name = elem->Attribute("tex");
            float mul = elem->FloatAttribute("mul");
            
            //this option available only for ibl
            ImageBasedLight::Ptr ibl = std::dynamic_pointer_cast<ImageBasedLight>(new_light);
            auto image_io = ImageIo::CreateImageIo();
            Texture::Ptr tex = image_io->LoadImage(tex_name.c_str());
            ibl->SetTexture(tex);
            ibl->SetMultiplier(mul);
        }
        else
        {
            THROW_EX("Invalid light type " + type);
        }

        RadeonRays::float3 p;
        RadeonRays::float3 d;
        RadeonRays::float3 r;

        p.x = elem->FloatAttribute("posx");
        p.y = elem->FloatAttribute("posy");
        p.z = elem->FloatAttribute("posz");

        d.x = elem->FloatAttribute("dirx");
        d.y = elem->FloatAttribute("diry");
        d.z = elem->FloatAttribute("dirz");

        r.x = elem->FloatAttribute("radx");
        r.y = elem->FloatAttribute("rady");
        r.z = elem->FloatAttribute("radz");

        new_light->SetPosition(p);
        new_light->SetDirection(d);
        new_light->SetEmittedRadiance(r);
        m_scene->AttachLight(new_light);
        elem = elem->NextSiblingElement("light");
    }
}

void Render::LoadSppXml(const std::string& file_name)
{
    tinyxml2::XMLDocument doc;
    doc.LoadFile(file_name.c_str());
    auto root = doc.FirstChildElement("spp_list");

    if (!root)
    {
        THROW_EX("Failed to open lights set file.")
    }

    tinyxml2::XMLElement* elem = root->FirstChildElement("spp");

    m_spp.clear();

    while (elem)
    {
        int spp = (int)elem->Int64Attribute("iter_num");
        m_spp.insert(spp);
        elem = elem->NextSiblingElement("spp");
    }
}

void Render::UpdateCameraSettings(const CameraInfo& cam_state)
{
    if (cam_state.aperture != m_camera->GetAperture())
    {
        m_camera->SetAperture(cam_state.aperture);
    }

    if (cam_state.focal_length != m_camera->GetFocalLength())
    {
        m_camera->SetFocalLength(cam_state.focal_length);
    }

    if (cam_state.focus_distance != m_camera->GetFocusDistance())
    {
        m_camera->SetFocusDistance(cam_state.focus_distance);
    }

    auto cur_pos = m_camera->GetPosition();
    auto at = m_camera->GetForwardVector();
    auto up = m_camera->GetUpVector();

    if (cur_pos != cam_state.pos ||
        at != cam_state.at ||
        up != cam_state.up)
    {
        m_camera->LookAt(cam_state.pos, cam_state.at, cam_state.up);
    }
}

void Render::SaveOutput(OutputDesc desc,
                        const std::string& file_dir,
                        int cam_index,
                        int spp)
{
    OIIO_NAMESPACE_USING;

    std::stringstream ss;

    ss << "cam_" << cam_index << "_"
        << desc.name << "_spp_" << spp << ".png";

    std::filesystem::path path = file_dir;

    if (!path.has_filename())
    {
        THROW_EX("incorrect output path");
    }

    path.append(ss.str());

    std::vector<RadeonRays::float3> output_data;
    std::vector<RadeonRays::float3> image_data;
    auto output = m_renderer->GetOutput(desc.type);
    auto width = output->width();
    auto height = output->height();

    assert(output);

    auto buffer = static_cast<Baikal::ClwOutput*>(output)->data();
    output_data.resize(buffer.GetElementCount());
    image_data.resize(buffer.GetElementCount());

    output->GetData(output_data.data());

    for (auto y = 0u; y < height; ++y)
    {
        for (auto x = 0u; x < width; ++x)
        {
            float3 val = output_data[(height - 1 - y) * width + x];
            val *= (1.f / val.w);
            image_data[y * width + x].x = val.x; // std::pow(val.x, 1.f / 2.2f);
            image_data[y * width + x].y = val.y;  //std::pow(val.y, 1.f / 2.2f);
            image_data[y * width + x].z = val.z;  //std::pow(val.z, 1.f / 2.2f);
        }
    }

    std::unique_ptr<ImageOutput> out(ImageOutput::create(ss.str()));

    if (!out)
    {
        THROW_EX("Can't create image file on disk");
    }

    ImageSpec spec(width, height, 3, TypeDesc::FLOAT);
    out->open(path.string(), spec);
    out->write_image(TypeDesc::FLOAT, image_data.data(), sizeof(float3));
    out->close();
}

void Render::GenerateDataset(const std::string &path)
{
    using namespace RadeonRays;

    int counter = 1;
    for (const auto &cam_state: m_camera_states)
    {
        if (!m_camera)
        {
            m_camera = Baikal::PerspectiveCamera::Create(cam_state.at, cam_state.pos, cam_state.up);
            m_scene->SetCamera(m_camera);
            m_camera->SetSensorSize(RadeonRays::float2(0.036f, 0.036f));
            m_camera->SetDepthRange(RadeonRays::float2(0.0f, 100000.f));

            m_camera->SetSensorSize(RadeonRays::float2(0.036f, 0.036f));
            m_camera->SetDepthRange(RadeonRays::float2(0.0f, 100000.f));
            m_camera->SetFocalLength(0.035f);
            m_camera->SetFocusDistance(1.f);
            m_camera->SetAperture(0.f);

            m_scene->SetCamera(m_camera);
        }

        UpdateCameraSettings(cam_state);

        for (const auto& output: m_outputs)
        {
            output->Clear(RadeonRays::float3(.0f, .0f, .0f, .0f));
        }

        m_controller->CompileScene(m_scene);
        auto& scene = m_controller->GetCachedScene(m_scene);

        for (auto i = 0u; i < kNumIterations; i++)
        {
            m_renderer->Render(scene);

            if (m_spp.find(i + 1) != m_spp.end())
            {
                for (const auto& output : outputs_collection)
                {
                    SaveOutput(output,
                               path,
                               counter,
                               i + 1);
                }
            }
        }

        counter++;
    }
}
