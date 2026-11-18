
#include "VideoBackends/Metal/MTLShader.h"

#include "Common/Logging/Log.h"

#include "VideoBackends/Metal/MTLObjectCache.h"
#include "VideoBackends/Metal/MTLUtil.h"

Metal::Shader::Shader(ShaderStage stage, std::string msl, MRCOwned<id<MTLFunction>> shader,
                      std::string glsl)
    : AbstractShader(stage), m_msl(std::move(msl)), m_shader(std::move(shader)),
      m_glsl(std::move(glsl))
{
  // SPIRV-Cross declares the view mask buffer in every multiview shader.
  m_multiview = m_msl.find("spvViewMask") != std::string::npos;
}

Metal::Shader::~Shader()
{
  g_object_cache->ShaderDestroyed(this);
}

AbstractShader::BinaryData Metal::Shader::GetBinary() const
{
  return BinaryData(m_msl.begin(), m_msl.end());
}

id<MTLFunction> Metal::Shader::GetMultiviewShader() const
{
  if (m_multiview)
    return m_shader;

  std::lock_guard lock(m_multiview_mutex);
  if (m_multiview_variant_tried)
    return m_multiview_variant;
  m_multiview_variant_tried = true;

  if (m_glsl.empty())
  {
    ERROR_LOG_FMT(VIDEO, "Metal: no GLSL to make a multiview variant of {}",
                  [[m_shader label] UTF8String]);
    return nil;
  }

  @autoreleasepool
  {
    std::optional<std::string> msl =
        Util::TranslateShaderToMSL(GetStage(), m_glsl, nullptr, /*force_multiview=*/true);
    if (!msl)
    {
      ERROR_LOG_FMT(VIDEO, "Metal: failed to translate the multiview variant of {}",
                    [[m_shader label] UTF8String]);
      return nil;
    }

    NSError* err = nullptr;
    auto lib = MRCTransfer([g_device newLibraryWithSource:[NSString stringWithUTF8String:msl->c_str()]
                                                  options:nil
                                                    error:&err]);
    if (err)
    {
      ERROR_LOG_FMT(VIDEO, "Metal: failed to compile the multiview variant of {}: {}",
                    [[m_shader label] UTF8String], [[err localizedDescription] UTF8String]);
      return nil;
    }
    m_multiview_variant = MRCTransfer([lib newFunctionWithName:@"main0"]);
    if (m_multiview_variant)
    {
      [m_multiview_variant
          setLabel:[NSString stringWithFormat:@"%@ (multiview)", [m_shader label]]];
    }
  }
  return m_multiview_variant;
}
