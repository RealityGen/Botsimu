/************************************************************************************

Filename    :   Render_GL_Device.h
Content     :   RenderDevice implementation header for OpenGL
Created     :   September 10, 2012
Authors     :   Andrew Reisse, David Borel

Copyright   :   Copyright (c) Facebook Technologies, LLC and its affiliates. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

************************************************************************************/

#ifndef OVR_Render_GL_Device_h
#define OVR_Render_GL_Device_h

#include "../Render/Render_Device.h"

#if defined(OVR_OS_WIN32)
    #include "Kernel/OVR_Win32_IncludeWindows.h"
    #include <GL/CAPI_GLE.h>
    //#include <GL/gl.h>
    //#include <GL/glext.h>
    //#include <GL/wglext.h>
#elif defined(OVR_OS_MAC)
    #include <GL/CAPI_GLE.h>
    //#include <OpenGL/gl3.h>
    //#include <OpenGL/gl3ext.h>
#else
    #include <GL/CAPI_GLE.h>
    //#include <GL/gl.h>
    //#include <GL/glext.h>
    #include <GL/glx.h>
#endif

#include "OVR_CAPI_GL.h"
#include "Util/Util_GL_Blitter.h"

namespace OVR { namespace Render { namespace GL {


extern void InitGLExtensions();


//// GLVersionAndExtensions
//
// FIXME: CODE DUPLICATION WARNING
// Right now we have this same code in CommonSrc and in CAPI::GL.
// At some point we need to consolidate these, in Kernel or Util.
// Be sure to update both locations for now!
//
// This class needs to be initialized at runtime with GetGLVersionAndExtensions,
// after an OpenGL context has been created. It must be re-initialized any time
// a new OpenGL context is created, as the new context may differ in version or
// supported functionality.
class GLVersionAndExtensions
{
public:
    // Version information
    int         MajorVersion;        // Best guess at major version
    int         MinorVersion;        // Best guess at minor version
    int         WholeVersion;        // Equals ((MajorVersion * 100) + MinorVersion). Example usage: if(glv.WholeVersion >= 302) // If OpenGL v3.02+ ...
    bool        IsGLES;              // Open GL ES?
    bool        IsCoreProfile;       // Is the current OpenGL context a core profile context? Its trueness may be a false positive but will never be a false negative.

    // Extension information
    bool        SupportsVAO;         // Supports Vertex Array Objects?
    bool        SupportsDrawBuffers; // Supports Draw Buffers?
    const char* Extensions;          // Other extensions string (will not be null)

    GLVersionAndExtensions()
      : MajorVersion(0),
        MinorVersion(0),
        WholeVersion(0),
        IsGLES(false),
        IsCoreProfile(false),
        SupportsVAO(false),
        SupportsDrawBuffers(false),
        Extensions("")
    {
    }

    bool HasGLExtension(const char* searchKey) const;

protected:
    friend void GetGLVersionAndExtensions(GLVersionAndExtensions& versionInfo);

    void ParseGLVersion();
    void ParseGLExtensions();
};

void GetGLVersionAndExtensions(GLVersionAndExtensions& versionInfo);



//// DebugCallback
//
// Used for high level usage and control of the various OpenGL debug output extensions. This is useful for 
// intercepting all OpenGL errors in a single place.
// This functionality is specific to OpenGL and no analog exists in DirectX, as DirectX doesn't support
// debug callbacks.
//
// Example basic usage:
//     DebugCallback glDebug;
//
//     <initialize OpenGL context>
//     glDebug.Initialize();
//     glDebug.SetMinSeverity(SeverityMedium, SeverityHigh);
//     <use OpenGL. Debug output will be logged by default.>
//     glDebug.Shutdown();
//     <destroy OpenGL context>
//
// There are three OpenGL API debug interfaces, each being an evolution of its predecessor:
//     AMD_debug_output - https://www.opengl.org/registry/specs/AMD/debug_output.txt
//     ARB_debug_output - https://www.opengl.org/registry/specs/ARB/debug_output.txt
//     KHR_debug        - https://www.opengl.org/registry/specs/KHR/debug.txt
//
// If the AMD_debug_output functionality is present in the OpenGL headers, GL_AMD_debug_output will be defined by glext.h.
// If the ARB_debug_output functionality is present in the OpenGL headers, GL_ARB_debug_output will be defined by glext.h.
// If the KHR_debug functionality is present in the OpenGL headers, GL_KHR_debug will be defined.
//
// As of at least XCode 5.1, debug functionality isn't yet supported by Macintosh OS X.
// KHR_debug is part of the OpenGL 4.3 core profile. It uses the same interface as the ARB extension along with some additions.
// The KHR_debug functionality doesn't include an API suffix (e.g. you would call glDebugMessageCallback).
// OpenGL ES supports KHR debug callbacks as of v3.1. However, for OpenGL ES entry points use the "KHR" suffix (e.g. glDebugMessageCallbackKHR).
// With the KHR version you can control debug messages at runtime with glEnable/glDisable(DEBUG_OUTPUT).
// The KHR_debug functionality requires that the OpenGL context be created with the CONTEXT_FLAG_DEBUG_BIT.
//     Windows wglCreateContextAttribsARB should be done with WGL_CONTEXT_DEBUG_BIT_ARB with in WGL_CONTEXT_FLAGS.
//     Linux glXCreateContext should be done with GLX_CONTEXT_DEBUG_BIT_ARB set in GLX_CONTEXT_FLAGS.


class DebugCallback
{
public:
    DebugCallback();
   ~DebugCallback();

    // Initialize must be called after the OpenGL context is created.
    void Initialize();

    // Shutdown must be called before the OpenGL context is destroyed.
    void Shutdown();

    enum Implementation
    {
        ImplementationNone,
        ImplementationAMD,      // Oldest version, deprecated by later versions.
        ImplementationARB,      // ARB version, deprecated by KHR version.
        ImplementationKHR       // OpenGL 4.3+ core profile version.
    };

    // Will return ImplementationNone until Initialize has been called, at which point it will return the version used.
    Implementation GetImplementation() const;

    // Maps to glEnable(GL_DEBUG_OUTPUT) when it is available, else does nothing. This controls debug output at the driver
    // level and can be used, for example, to temporarily disable debug output that some other application entity enabled
    // via glDebugMessageCallback. In practice this is available only when KHR_debug is available
    void EnableGLDebug(bool enabled);

    enum Severity // These are a mirror of the OpenGL types.
    {
        SeverityNone = 0,
        SeverityNotification,
        SeverityLow,
        SeverityMedium,
        SeverityHigh,
        SeverityDisabled        // When min severity is set to this level, it is never logged or assertion-failed.
    };

    // Set the severity required before we log or assert on a debug message. Default is SeverityHigh/SeverityHigh.
    void SetMinSeverity(Severity minLogSeverity, Severity minAssertSeverity);

    // Returns the debug callback currently used by OpenGL.
    // This works for both ARB and KHR implementations. The same debug callback is used by OpenGL implementations for both.
    // However, you can call GetImplementation to see which of the two we are using. This functionality is not available 
    // with the AMD implementation. Returns false and sets debugCallback and userParam to NULL if there is no existing callback.
    bool GetGLDebugCallback(PFNGLDEBUGMESSAGECALLBACKPROC* debugCallback, const void** userParam) const;

protected:
    void DebugCallbackInternal(Severity s, const char* pSource, const char* pType, GLuint id, const char* pSeverity, const char* message) const;

    // ARB and KHR debug handler
    static void GLAPIENTRY DebugMessageCallback(GLenum Source, GLenum Type, GLuint Id, GLenum Severity, GLsizei Length, const GLchar* Message, const void* UserParam);
    static const char*   GetSource(GLenum Source);
    static const char*   GetType(GLenum Type);
    static const char*   GetSeverity(GLenum Severity);

    // AMD handler 
    static void GLAPIENTRY DebugMessageCallbackAMD(GLuint id, GLenum category, GLenum severity, GLsizei length, const GLchar* message, void* userParam);
    static const char*   GetCategoryAMD(GLenum Category);

protected:
    bool                             Initialized;
    int                              MinLogSeverity;                // Minimum severity for us to log the event.
    int                              MinAssertSeverity;             // Minimum severity for us to assertion-fail the event.
  //PFNGLDEBUGMESSAGECALLBACKPROC    glDebugMessageCallback;
  //PFNGLDEBUGMESSAGECONTROLPROC     glDebugMessageControl;
  //PFNGLDEBUGMESSAGECALLBACKARBPROC glDebugMessageCallbackARB;     // glDebugMessageCallbackARB is the same as glDebugMessageCallback and may not need to be a separate variable.
  //PFNGLDEBUGMESSAGECONTROLARBPROC  glDebugMessageControlARB;
  //PFNGLDEBUGMESSAGECALLBACKAMDPROC glDebugMessageCallbackAMD;
  //PFNGLDEBUGMESSAGEENABLEAMDPROC   glDebugMessageControlAMD;
};





class RenderDevice;

class Buffer : public Render::Buffer
{
public:
    RenderDevice* Ren;
    size_t        Size;
    GLenum        Use;
    GLuint        GLBuffer;

public:
    Buffer(RenderDevice* r) : Ren(r), Size(0), Use(0), GLBuffer(0) {}
    ~Buffer();

    GLuint         GetBuffer() { return GLBuffer; }

    virtual size_t GetSize() { return Size; }
    virtual void*  Map(size_t start, size_t size, int flags = 0);
    virtual bool   Unmap(void *m);
    virtual bool   Data(int use, const void* buffer, size_t size);
};

class Texture : public Render::Texture
{
public:
    ovrSession      Session;
    RenderDevice*   Ren;
    ovrTextureSwapChain TextureChain;
    ovrMirrorTexture MirrorTexture;
    int             Width, Height, Samples;
    uint64_t        Format;
    GLuint          TexId;

    Texture(ovrSession session, RenderDevice* r, uint64_t fmt, int w, int h, int samples);
    ~Texture();

    GLuint GetTexId()
    {
        if (TextureChain)
        {
            int currentIndex;
            ovr_GetTextureSwapChainCurrentIndex(Session, TextureChain, &currentIndex);

            unsigned int id = 0;
            ovr_GetTextureSwapChainBufferGL(Session, TextureChain, currentIndex, &id);

            return id;
        }
        else
        {
            return TexId;
        }
    }

    virtual int GetWidth() const override{ return Width; }
    virtual int GetHeight() const override{ return Height; }
    virtual int GetSamples() const override{ return Samples; }
    virtual uint64_t GetFormat() const override{ return Format; }

    virtual void SetSampleMode(int);

    virtual void Set(int slot, ShaderStage stage = Shader_Fragment) const;

    virtual ovrTextureSwapChain Get_ovrTextureSet() override{ return TextureChain; }

    virtual void GenerateMips() override;
    virtual void Commit() override;
};

class Shader : public Render::Shader
{
public:
    GLuint      GLShader;

    Shader(RenderDevice*, ShaderStage st, GLuint s) : Render::Shader(st), GLShader(s) {}
    Shader(RenderDevice*, ShaderStage st, const char* src) : Render::Shader(st), GLShader(0)
    {
        Compile(src);
    }

    ~Shader();
    bool Compile(const char* src);

    GLenum GLStage() const
    {
        switch (Stage)
        {
        default:  OVR_ASSERT(0); return GL_NONE;
        case Shader_Vertex: return GL_VERTEX_SHADER;
        case Shader_Fragment: return GL_FRAGMENT_SHADER;
        }
    }

    //void Set(PrimitiveType prim) const;
    //void SetUniformBuffer(Render::Buffer* buffers, int i = 0);
};

class ShaderSet : public Render::ShaderSet
{
public:
    GLuint Prog;

    struct Uniform
    {
        String Name;
        int    Location, Size;
        int    Type; // currently number of floats in vector

        Uniform() : Name(), Location(0), Size(0), Type(0){}
    };
    std::vector<Uniform> UniformInfo;

    int     ProjLoc, ViewLoc, GlobalTintLoc;
    int     TexLoc[8];
    bool    UsesLighting;
    int     LightingVer;

    ShaderSet();
    ~ShaderSet();

    virtual void SetShader(Render::Shader *s);
    virtual void UnsetShader(int stage);

    virtual void Set(PrimitiveType prim) const;

    // Set a uniform (other than the standard matrices). It is undefined whether the
    // uniforms from one shader occupy the same space as those in other shaders
    // (unless a buffer is used, then each buffer is independent).     
    virtual bool SetUniform(const char* name, int n, const float* v);
    virtual bool SetUniform4x4f(const char* name, const Matrix4f& m);

    bool Link();
};

class RBuffer : public RefCountBase<RBuffer>
{
 public:
    int    Width, Height;
    GLuint BufId;

    RBuffer(GLenum format, GLint w, GLint h);
    ~RBuffer();
};

class RenderDevice : public Render::RenderDevice
{
    Ptr<Shader>        VertexShaders[VShader_Count];
    Ptr<Shader>        FragShaders[FShader_Count];

    Ptr<ShaderFill>         DefaultFill;
    Ptr<Fill>               DefaultTextureFill;
    Ptr<Fill>               DefaultTextureFillAlpha;
    Ptr<Fill>               DefaultTextureFillPremult;

    Matrix4f    Proj;
    Vector4f    GlobalTint;

    GLuint Vao;

    Ptr<GLUtil::Blitter>    Blitter;

protected:
    Ptr<Texture>                   CurRenderTarget;
    std::vector<Ptr<Texture> >     DepthBuffers;
    GLuint                         CurrentFbo;
    GLuint                         MsaaFbo;
    GLVersionAndExtensions         GLVersionInfo;
    DebugCallback                  DebugCallbackControl;
    const LightingParams*          Lighting;

public:
    RenderDevice(ovrSession session, const RendererParams& p);
    virtual ~RenderDevice();

    virtual void DeleteFills() override;

    virtual void Shutdown() override;
    
    virtual void FillTexturedRect(float left, float top, float right, float bottom, float ul, float vt, float ur, float vb, Color c, Ptr<OVR::Render::Texture> tex, const Matrix4f* view, bool premultAlpha = false) override;

    virtual void SetViewport(const Recti& vp) override;
        
    virtual void Flush() override;

    virtual void Clear(float r = 0, float g = 0, float b = 0, float a = 1, float depth = 1,
                       bool clearColor = true, bool clearDepth = true, int faceIndex = -1) override;

    virtual void BeginRendering() override;
    virtual void SetDepthMode(bool enable, bool write, CompareFunc func = Compare_Less) override;
    virtual void SetWorldUniforms(const Matrix4f& proj, const Vector4f& globalTint) override;

    virtual Texture* GetDepthBuffer(int w, int h, int ms, TextureFormat depthFormat = Texture_Depth32f) override;

    virtual void ResolveMsaa(OVR::Render::Texture* msaaTex, OVR::Render::Texture* outputTex) override;

    virtual void SetCullMode(CullMode cullMode) override;

    virtual bool Present(bool withVsync)  override{ OVR_UNUSED(withVsync); return true; };
    virtual void SetRenderTarget(Render::Texture* color,
                                 Render::Texture* depth = NULL, Render::Texture* stencil = NULL, int faceIndex = -1) override;

    virtual void SetLighting(const LightingParams* lt) override;

    virtual void Blt(Render::Texture* texture) override;
    virtual void Blt(Render::Texture* texture, uint32_t topLeftX, uint32_t topLeftY, uint32_t width, uint32_t height) override;
    virtual void BltToTex(Render::Texture* src, Render::Texture* dest) override;
    virtual void BltFlipCubemap(Render::Texture* src, Render::Texture* temp) override;

    // Overridden to apply proper blend state.
    virtual void FillRect(float left, float top, float right, float bottom, Color c, const Matrix4f* view = NULL) override;
    virtual void RenderText(const struct Font* font, const char* str, float x, float y, float size, Color c, const Matrix4f* view = NULL) override;
    virtual void FillGradientRect(float left, float top, float right, float bottom, Color col_top, Color col_btm, const Matrix4f* view) override;
    virtual void RenderImage(float left, float top, float right, float bottom, ShaderFill* image, unsigned char alpha = 255, const Matrix4f* view = NULL) override;

    virtual void Render(const Matrix4f& matrix, Model* model) override;
    virtual void Render(const Fill* fill, Render::Buffer* vertices, Render::Buffer* indices,
                        const Matrix4f& matrix, int offset, int count, PrimitiveType prim = Prim_Triangles) override;
    virtual void RenderWithAlpha(const Fill* fill, Render::Buffer* vertices, Render::Buffer* indices,
                                 const Matrix4f& matrix, int offset, int count, PrimitiveType prim = Prim_Triangles) override;

    virtual Buffer* CreateBuffer() override;
    virtual Texture* CreateTexture(uint64_t format, int width, int height, const void* data, int mipcount = 1, ovrResult* error = nullptr) override;
    virtual ShaderSet* CreateShaderSet() override { return new ShaderSet; }

    virtual Fill *GetSimpleFill(int flags = Fill::F_Solid) override;
    virtual Fill *GetTextureFill(Render::Texture* tex, bool useAlpha = false, bool usePremult = false) override;

    virtual Shader *LoadBuiltinShader(ShaderStage stage, int shader) override;

    void SetTexture(Render::ShaderStage, int slot, const Texture* t);

    bool SaveCubemapTexture(Render::Texture* tex, Vector3f transl, const std::string& filePath, std::string* error) override;
};



}}}

#endif
