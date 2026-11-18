/* Weapon material pass, used only on the verified SoF2 renderer's stereo draws.
 * Native UVs, geometry, depth, and dynamic-light/fog passes are retained. */
enum { MATERIAL_WEAPONS=37, MATERIAL_SLOTS=8, MATERIAL_CACHE=12 };
typedef struct {
    GLuint nativeTexture, albedo, parameters;
    unsigned lastDraw, logged;
    int unavailable, weapon, slot;
} WeaponMaterial;
static struct {
    HGLRC context;
    GLuint program;
    GLuint normalBuffer, normalTexture;
    WeaponMaterial slots[MATERIAL_WEAPONS][MATERIAL_SLOTS];
    WeaponMaterial *cache[MATERIAL_CACHE];
    GLint normalArray, fogMode, worldUp, lightForward, style;
    DWORD refreshed;
    unsigned frame, cached;
    int attempted;
    PFNGLUSEPROGRAMPROC useProgram;
    PFNGLUNIFORM1IPROC uniform1i;
    PFNGLUNIFORM3FPROC uniform3f;
    PFNGLACTIVETEXTUREPROC activeTexture;
    PFNGLBINDBUFFERPROC bindBuffer;
    PFNGLBUFFERDATAPROC bufferData;
} weaponMaterial;

static const char weaponVertexSource[] =
    "#version 140\n"
    "#extension GL_ARB_compatibility : enable\n"
    "uniform samplerBuffer meshNormals;\n"
    "uniform int normalArray;\n"
    "varying vec3 eyePoint, smoothNormal, light;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "  gl_ClipVertex = gl_ModelViewMatrix * gl_Vertex;\n"
    "  gl_Position = ftransform();\n"
    "  eyePoint = gl_ClipVertex.xyz;\n"
    "  vec3 normal = normalArray == 2 ? texelFetch(meshNormals, gl_VertexID).xyz : gl_Normal;\n"
    "  smoothNormal = gl_NormalMatrix * normal;\n"
    "  light = gl_Color.rgb;\n"
    "  uv = (gl_TextureMatrix[0] * gl_MultiTexCoord0).st;\n"
    "}\n";

static const char weaponFragmentSource[] =
    "#version 140\n"
    "#extension GL_ARB_compatibility : enable\n"
    "uniform sampler2D albedo, parameters;\n"
    "uniform int normalArray, fogMode, style;\n"
    "uniform vec3 worldUp, lightForward;\n"
    "varying vec3 eyePoint, smoothNormal, light;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "  vec3 dx = dFdx(eyePoint), dy = dFdy(eyePoint);\n"
    "  vec2 ux = dFdx(uv), uy = dFdy(uv);\n"
    "  vec3 view = normalize(-eyePoint);\n"
    "  vec3 n = normalize(normalArray != 0 ? smoothNormal : cross(dx, dy));\n"
    "  if (dot(n, view) < 0.0) n = -n;\n"
    "  float det = ux.x * uy.y - ux.y * uy.x;\n"
    "  vec4 data = texture2D(parameters, uv);\n"
    "  vec3 mapped = normalize(data.rgb * 2.0 - 1.0);\n"
    /* The source's inward normals mark recessed engraving. Limit the light
     * reaching those texels rather than reflecting from a back-facing plane. */
    "  float visible = smoothstep(-0.38, 0.42, mapped.z);\n"
    "  if (abs(det) > 1e-12) {\n"
    "    vec3 tangent = (dx * uy.y - dy * ux.y) * sign(det);\n"
    "    vec3 bitangent = (dy * ux.x - dx * uy.x) * sign(det);\n"
    "    tangent = normalize(tangent - n * dot(tangent, n));\n"
    "    vec3 b = normalize(cross(n, tangent));\n"
    "    b *= sign(dot(b, bitangent));\n"
    /* The native texture coordinates use a top-first image (1 - source V). */
    "    n = normalize(tangent * mapped.x - b * mapped.y + n * max(mapped.z, 0.0));\n"
    "  }\n"
    "  vec4 surface = texture2D(albedo, uv);\n"
    "  float metallic = surface.a, roughness = clamp(data.a, 0.08, 1.0);\n"
    "  vec3 base = surface.rgb;\n"
    "  vec3 reflected = reflect(-view, n);\n"
    /* Broad sky fill, an extended cool reflection above the shooter and
     * warm ground bounce. A strip source brings out the satin stock. */
    "  float elevation = dot(reflected, worldUp);\n"
    "  float sky = smoothstep(-0.82, 0.78, elevation);\n"
    "  vec2 profile = vec2(elevation, dot(reflected, lightForward));\n"
    "  profile *= inversesqrt(max(dot(profile, profile), 1e-6));\n"
    "  float key = exp2((dot(profile, vec2(0.675, -0.738)) - 1.0) / (0.099 + 0.10 * roughness));\n"
    "  float bounce = exp2((dot(profile, vec2(-0.33, -0.94)) - 1.0) / 0.095);\n"
    "  float ground = exp2((dot(profile, vec2(-0.999, -0.049)) - 1.0) / 0.20);\n"
    "  float facing = max(dot(n, view), 0.0);\n"
    /* Karis' analytic fit to the integrated specular BRDF. This accounts for
     * masking on rough surfaces and avoids a fixed bright Fresnel outline. */
    "  vec4 dfg = roughness * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);\n"
    "  float grazing = min(dfg.x * dfg.x, exp2(-9.28 * facing)) * dfg.x + dfg.y;\n"
    "  vec2 response = max(vec2(0.0), vec2(-1.04, 1.04) * grazing + dfg.zw);\n"
    "  float local = clamp(dot(light, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);\n"
    "  float scene = 0.63 + 0.60 * local;\n"
    "  float fill = style == 0 ? 0.56 + 0.19 * sky : 0.34 + 0.10 * sky;\n"
    "  vec3 diffuse = base * (1.0 - metallic) * fill;\n"
    "  vec3 coating = vec3((0.035 * response.x + 0.12 * response.y) * mix(0.23, 0.44, sky));\n"
    "  float side = max(-profile.y, 0.0);\n"
    "  coating += vec3(0.0042, 0.0035, 0.0031) * side * side + vec3(0.0020, 0.0024, 0.0025) * key;\n"
    /* This stock atlas island is the satin tube. Its V axis runs from the
     * receiver to the butt. Keep the adjoining pad and controls matte. */
    "  if (style == 2 && uv.x < 0.431 && uv.y > 0.326) {\n"
    "    float along = clamp((uv.y - 0.328) / 0.664, 0.0, 1.0);\n"
    "    vec2 direction = normalize(mix(vec2(0.865, 0.503), vec2(0.962, 0.274), along));\n"
    "    float phase = profile.x * direction.y - profile.y * direction.x;\n"
    "    float spread = mix(0.185, 0.065, step(0.0, phase)) + 0.065 * roughness;\n"
    "    float strip = exp2((dot(profile, direction) - 1.0) / spread);\n"
    "    coating += vec3(0.894, 1.0, 1.018) * (0.085 + 0.282 * along) * strip;\n"
    "    coating += vec3(0.0044, 0.0045, 0.0046) * key + vec3(0.0045, 0.0038, 0.0031) * ground;\n"
    "  }\n"
    "  vec3 metalLight = vec3(0.209, 0.212, 0.213) + vec3(0.431, 0.482, 0.483) * key\n"
    "                 + vec3(0.052, 0.005, 0.0) * bounce + vec3(0.079, 0.078, 0.077) * ground;\n"
    "  if (style == 0) metalLight += vec3(0.12 + 0.10 * sky);\n"
    "  vec3 metal = (base * response.x + 0.10 * response.y) * metalLight;\n"
    "  vec3 linear = clamp(scene * (diffuse * (0.22 + 0.78 * visible) + mix(coating, metal, metallic) * visible), 0.0, 1.0);\n"
    "  vec3 color = mix(linear * 12.92, 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, linear));\n"
    "  float distance = abs(eyePoint.z), fog = 1.0;\n"
    "  if (fogMode == 9729) fog = (gl_Fog.end - distance) * gl_Fog.scale;\n"
    "  if (fogMode == 2048) fog = exp(-distance * gl_Fog.density);\n"
    "  if (fogMode == 2049) { float d = distance * gl_Fog.density; fog = exp(-d*d); }\n"
    "  gl_FragColor = vec4(mix(gl_Fog.color.rgb, color, clamp(fog, 0.0, 1.0)), 1.0);\n"
    "}\n";

static GLuint CompileWeaponMaterial(void) {
    PFNGLCREATESHADERPROC createShader = (void *)wglGetProcAddress("glCreateShader");
    PFNGLSHADERSOURCEPROC shaderSource = (void *)wglGetProcAddress("glShaderSource");
    PFNGLCOMPILESHADERPROC compileShader = (void *)wglGetProcAddress("glCompileShader");
    PFNGLGETSHADERIVPROC getShader = (void *)wglGetProcAddress("glGetShaderiv");
    PFNGLGETSHADERINFOLOGPROC shaderLog = (void *)wglGetProcAddress("glGetShaderInfoLog");
    PFNGLDELETESHADERPROC deleteShader = (void *)wglGetProcAddress("glDeleteShader");
    PFNGLCREATEPROGRAMPROC createProgram = (void *)wglGetProcAddress("glCreateProgram");
    PFNGLATTACHSHADERPROC attachShader = (void *)wglGetProcAddress("glAttachShader");
    PFNGLLINKPROGRAMPROC linkProgram = (void *)wglGetProcAddress("glLinkProgram");
    PFNGLGETPROGRAMIVPROC getProgram = (void *)wglGetProcAddress("glGetProgramiv");
    PFNGLGETPROGRAMINFOLOGPROC programLog = (void *)wglGetProcAddress("glGetProgramInfoLog");
    PFNGLDELETEPROGRAMPROC deleteProgram = (void *)wglGetProcAddress("glDeleteProgram");
    PFNGLGETUNIFORMLOCATIONPROC uniform = (void *)wglGetProcAddress("glGetUniformLocation");
    weaponMaterial.useProgram = (void *)wglGetProcAddress("glUseProgram");
    weaponMaterial.uniform1i = (void *)wglGetProcAddress("glUniform1i");
    weaponMaterial.uniform3f = (void *)wglGetProcAddress("glUniform3f");
    weaponMaterial.activeTexture = (void *)wglGetProcAddress("glActiveTexture");
    weaponMaterial.bindBuffer = (void *)wglGetProcAddress("glBindBuffer");
    weaponMaterial.bufferData = (void *)wglGetProcAddress("glBufferData");
    PFNGLGENBUFFERSPROC genBuffers = (void *)wglGetProcAddress("glGenBuffers");
    PFNGLTEXBUFFERPROC texBuffer = (void *)wglGetProcAddress("glTexBuffer");
    if(!createShader || !shaderSource || !compileShader || !getShader || !shaderLog || !deleteShader ||
       !createProgram || !attachShader || !linkProgram || !getProgram || !programLog || !deleteProgram || !uniform ||
       !weaponMaterial.useProgram || !weaponMaterial.uniform1i || !weaponMaterial.uniform3f || !weaponMaterial.activeTexture)return 0;

    const char *sources[] = {weaponVertexSource, weaponFragmentSource};
    GLenum types[] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
    GLuint shaders[2] = {0}, program = 0;
    GLint ok;
    for(int i=0; i<2; ++i){
        shaders[i] = createShader(types[i]);
        if(!shaders[i])goto done;
        shaderSource(shaders[i], 1, &sources[i], NULL);
        compileShader(shaders[i]);
        getShader(shaders[i], GL_COMPILE_STATUS, &ok);
        if(!ok){
            char error[512] = {0};
            shaderLog(shaders[i], sizeof(error), NULL, error);
            LOG_WARN("Weapon material shader compile: %s", error);
            goto done;
        }
    }
    program = createProgram();
    if(!program)goto done;
    for(int i=0; i<2; ++i)attachShader(program, shaders[i]);
    linkProgram(program);
    getProgram(program, GL_LINK_STATUS, &ok);
    if(!ok){
        char error[512] = {0};
        programLog(program, sizeof(error), NULL, error);
        LOG_WARN("Weapon material program link: %s", error);
        deleteProgram(program);
        program = 0;
        goto done;
    }
    GLint previous;
    glGetIntegerv(GL_CURRENT_PROGRAM, &previous);
    weaponMaterial.useProgram(program);
    weaponMaterial.uniform1i(uniform(program, "albedo"), 0);
    weaponMaterial.uniform1i(uniform(program, "parameters"), 1);
    weaponMaterial.uniform1i(uniform(program, "meshNormals"), 2);
    weaponMaterial.normalArray = uniform(program, "normalArray");
    weaponMaterial.worldUp = uniform(program, "worldUp");
    weaponMaterial.lightForward = uniform(program, "lightForward");
    weaponMaterial.style = uniform(program, "style");
    weaponMaterial.fogMode = uniform(program, "fogMode");
    weaponMaterial.useProgram(previous);
    if(genBuffers && texBuffer && weaponMaterial.bindBuffer && weaponMaterial.bufferData){
        GLint previousTexture, previousBuffer;
        glGetIntegerv(GL_TEXTURE_BINDING_BUFFER, &previousTexture);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousBuffer);
        genBuffers(1, &weaponMaterial.normalBuffer);
        weaponMaterial.bindBuffer(GL_ARRAY_BUFFER, weaponMaterial.normalBuffer);
        weaponMaterial.bufferData(GL_ARRAY_BUFFER, 1024*16, NULL, GL_STREAM_DRAW);
        weaponMaterial.bindBuffer(GL_ARRAY_BUFFER, previousBuffer);
        glGenTextures(1, &weaponMaterial.normalTexture);
        glBindTexture(GL_TEXTURE_BUFFER, weaponMaterial.normalTexture);
        texBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, weaponMaterial.normalBuffer);
        glBindTexture(GL_TEXTURE_BUFFER, previousTexture);
    }
done:
    for(int i=0; i<2; ++i)if(shaders[i])deleteShader(shaders[i]);
    return program;
}

static GLuint LoadWeaponTexture(int weapon, int slot, char kind, GLenum internal) {
    typedef int (__cdecl *ReadFileFn)(const char *, void **);
    typedef void (__cdecl *FreeFileFn)(void *);
    char path[48];
    snprintf(path, sizeof(path), "sof2vr_ext/w%d/%c%d.bin", weapon, kind, slot);
    void *file = NULL;
    int bytes = ((ReadFileFn)(gameBase + 0x3ae10))(path, &file);
    if(!file)return 0;
    GLuint texture = 0;
    GLsizei size[2];
    if(bytes < 12 || memcmp(file, "S2M4", 4))goto done;
    memcpy(size, (const BYTE *)file + 4, sizeof(size));
    if(size[0]<1 || size[1]<1 || size[0]>2048 || size[1]>2048 || bytes!=12+size[0]*size[1]*4)goto done;
    glGenTextures(1, &texture);
    if(!texture)goto done;
    GLint previous, alignment;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, size[0], size[1], 0, GL_RGBA, GL_UNSIGNED_BYTE, (const BYTE *)file + 12);
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    GLint width = 0, height = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    if(width==size[0] && height==size[1]){
        PFNGLGENERATEMIPMAPPROC mipmaps = (void *)wglGetProcAddress("glGenerateMipmap");
        if(mipmaps)mipmaps(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmaps?GL_LINEAR_MIPMAP_LINEAR:GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        if(tracePoses)LOG_INFO("MATERIAL texture weapon=%d slot=%d map=%c %dx%d", weapon, slot, kind, width, height);
    }else{
        glDeleteTextures(1, &texture);
        texture = 0;
    }
    glBindTexture(GL_TEXTURE_2D, previous);
done:
    ((FreeFileFn)(gameBase + 0x3b0c0))(file);
    return texture;
}

static void RefreshWeaponTextureNames(void) {
    DWORD now = GetTickCount();
    if(weaponMaterial.refreshed && now-weaponMaterial.refreshed<500)return;
    weaponMaterial.refreshed = now;
    /* Verified image hash chains: name at +0, next at +0x40, GL name +0x58. */
    BYTE **images = (BYTE **)(gameBase + 0x7986b0);
    for(int weapon=0; weapon<MATERIAL_WEAPONS; ++weapon)
        for(int slot=0; slot<MATERIAL_SLOTS; ++slot)weaponMaterial.slots[weapon][slot].nativeTexture=0;
    for(int bucket=0; bucket<1024; ++bucket){
        for(BYTE *image=images[bucket]; image; image=*(BYTE **)(image+0x40)){
            const char *name=(const char *)image, prefix[]="sof2vr_ext/w";
            if(_strnicmp(name,prefix,sizeof(prefix)-1))continue;
            int weapon,slot,end=0;
            if(sscanf(name+sizeof(prefix)-1,"%2d/m%1d%n",&weapon,&slot,&end)!=2 || !end ||
               weapon<1 || weapon>=MATERIAL_WEAPONS || slot<0 || slot>=MATERIAL_SLOTS)continue;
            const char *suffix=name+sizeof(prefix)-1+end;
            if(!*suffix || !_stricmp(suffix,".png"))
                weaponMaterial.slots[weapon][slot].nativeTexture=*(GLuint *)(image+0x58);
        }
    }
}
static void CacheWeaponMaterial(WeaponMaterial *entry) {
    unsigned index=weaponMaterial.cached;
    if(index<MATERIAL_CACHE)++weaponMaterial.cached;
    else{
        index=0;
        for(unsigned i=1;i<weaponMaterial.cached;++i)
            if(weaponMaterial.cache[i]->lastDraw<weaponMaterial.cache[index]->lastDraw)index=i;
        WeaponMaterial *old=weaponMaterial.cache[index];
        glDeleteTextures(1,&old->albedo);
        glDeleteTextures(1,&old->parameters);
        old->albedo=old->parameters=0;
    }
    weaponMaterial.cache[index]=entry;
}
static void PrepareWeaponMaterials(void) {
    if(weaponMaterial.context!=eyeContext){
        memset(&weaponMaterial,0,sizeof(weaponMaterial));
        weaponMaterial.context=eyeContext;
    }
    ++weaponMaterial.frame;
    RefreshWeaponTextureNames();
    VRWeaponState state;
    if(!playerScene || !controlsEnabled || !GameVR_GetWeaponState(&state) ||
       state.weapon<1 || state.weapon>=MATERIAL_WEAPONS)return;
    /* Upload at most one visible weapon's material pair per eye frame.
     * Native shading covers first use and hardware without this pass. */
    for(int slot=0; slot<MATERIAL_SLOTS; ++slot){
        WeaponMaterial *entry=&weaponMaterial.slots[state.weapon][slot];
        if(!entry->nativeTexture || entry->albedo || entry->unavailable)continue;
        if(!weaponMaterial.attempted){
            weaponMaterial.attempted=1;
            weaponMaterial.program=CompileWeaponMaterial();
            if(!weaponMaterial.program)LOG_WARN("Weapon material pass unavailable, using native shading");
        }
        if(!weaponMaterial.program)return;
        entry->parameters=LoadWeaponTexture(state.weapon,slot,'p',GL_RGBA8);
        if(entry->parameters)entry->albedo=LoadWeaponTexture(state.weapon,slot,'c',GL_SRGB8_ALPHA8);
        if(!entry->albedo){
            glDeleteTextures(1,&entry->parameters);
            entry->parameters=0;
            entry->unavailable=1;
        }else{
            entry->lastDraw=weaponMaterial.frame;
            entry->weapon=state.weapon;entry->slot=slot;
            CacheWeaponMaterial(entry);
        }
        break;
    }
}

typedef struct {GLint texture, program, normalTexture; GLuint nativeTexture; int customNormals;} WeaponMaterialDrawState;

static const void *WeaponMaterialNormals(const void *vertices, int scratch) {
    typedef struct {int end; const float *data; int stride; const float *constant;} NormalSpan;
    static float normals[1024][4];
    const void *native = gameBase+0x7bbf40;
    if(vertices!=gameBase+0x7b7f40 && !scratch)return NULL;
    int vertexCount = *(int *)(gameBase+0x7d3f54);
    int spans = *(int *)(gameBase+0x7daf74);
    if(vertexCount<1 || vertexCount>1024 || spans<0 || spans>256)return NULL;
    if(!spans)return native;
    /* The native iterator can assemble positions in its NV array-range
     * buffer while keeping normals in a separate span list. Expand exactly
     * those spans in vertex order, as the native normal iterator does. */
    const NormalSpan *source = (const NormalSpan *)(gameBase+0x7daf78);
    int index = 0;
    for(int span=0; span<spans && index<vertexCount; ++span){
        const NormalSpan *s = &source[span];
        int end = s->end < vertexCount ? s->end : vertexCount;
        if(end<=index)return NULL;
        const float *normal = s->constant ? s->constant : s->data;
        if(!normal || (!s->constant && (s->stride<3 || s->stride>32)))return NULL;
        for(; index<end; ++index){
            memcpy(normals[index], normal, sizeof(float)*3);
            if(!s->constant)normal += s->stride;
        }
    }
    return index==vertexCount ? normals : NULL;
}

static int BeginWeaponMaterial(GLenum mode, WeaponMaterialDrawState *saved) {
    if(!eyeActive || !activeSkyEye || !weaponMaterial.program || mode!=GL_TRIANGLES)return 0;
    /* Native GL_Bind maintains these texture-unit and binding values. */
    if(*(int *)(gameBase + 0x7999a8)!=0)return 0;
    GLuint bound = *(GLuint *)(gameBase + 0x7999a0);
    if(!bound)return 0;
    WeaponMaterial *entry=NULL;
    for(unsigned i=0;i<weaponMaterial.cached;++i){
        WeaponMaterial *candidate=weaponMaterial.cache[i];
        if(candidate->nativeTexture==bound){entry=candidate;break;}
    }
    if(!entry || glIsEnabled(GL_BLEND))return 0;
    int weapon=entry->weapon,slot=entry->slot;
    GLint actual, active;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &actual);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    if(active!=GL_TEXTURE0 || (GLuint)actual!=bound || !glIsEnabled(GL_TEXTURE_2D))return 0;
    saved->nativeTexture = bound;
    glGetIntegerv(GL_CURRENT_PROGRAM, &saved->program);
    saved->customNormals = 0;
    void *vertices;
    glGetPointerv(GL_VERTEX_ARRAY_POINTER, &vertices);
    int normals = glIsEnabled(GL_NORMAL_ARRAY);
    if(!normals){
        GLint buffer, stride, format, size;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
        glGetIntegerv(GL_VERTEX_ARRAY_STRIDE, &stride);
        glGetIntegerv(GL_VERTEX_ARRAY_TYPE, &format);
        glGetIntegerv(GL_VERTEX_ARRAY_SIZE, &size);
        uintptr_t begin=*(uintptr_t *)(gameBase+0x7aadb8), end=*(uintptr_t *)(gameBase+0x7aadbc);
        int scratch=begin && (uintptr_t)vertices>=begin && (uintptr_t)vertices<end;
        const void *data=NULL;
        if(!buffer && stride==16 && format==GL_FLOAT && size==3)data=WeaponMaterialNormals(vertices,scratch);
        if(data && weaponMaterial.normalTexture){
            /* Index the same smooth normals in the shader without changing
             * any of SoF2's client arrays while the native batch is active. */
            int count = *(int *)(gameBase+0x7d3f54);
            weaponMaterial.bindBuffer(GL_ARRAY_BUFFER, weaponMaterial.normalBuffer);
            weaponMaterial.bufferData(GL_ARRAY_BUFFER, (GLsizeiptr)count*16, data, GL_STREAM_DRAW);
            weaponMaterial.bindBuffer(GL_ARRAY_BUFFER, 0);
            weaponMaterial.activeTexture(GL_TEXTURE2);
            glGetIntegerv(GL_TEXTURE_BINDING_BUFFER, &saved->normalTexture);
            glBindTexture(GL_TEXTURE_BUFFER, weaponMaterial.normalTexture);
            saved->customNormals = 1;
            normals = 2;
        }
    }
    weaponMaterial.activeTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved->texture);
    glBindTexture(GL_TEXTURE_2D, entry->parameters);
    weaponMaterial.activeTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, entry->albedo);
    weaponMaterial.useProgram(weaponMaterial.program);
    weaponMaterial.uniform1i(weaponMaterial.normalArray, normals);
    GLint fog = 0;
    if(glIsEnabled(GL_FOG))glGetIntegerv(GL_FOG_MODE, &fog);
    weaponMaterial.uniform1i(weaponMaterial.fogMode, fog);
    const float (*axis)[3] = activeSkyEye->axis;
    weaponMaterial.uniform3f(weaponMaterial.worldUp, -axis[1][2], axis[2][2], -axis[0][2]);
    /* Hold the reflection profile to body heading across both eye poses. */
    const float *forward = playerScene && controlsEnabled ? playerBase.axis[0] : axis[0];
    float x = forward[0], y = forward[1];
    weaponMaterial.uniform3f(weaponMaterial.lightForward, -axis[1][0]*x-axis[1][1]*y,
                        axis[2][0]*x+axis[2][1]*y, -axis[0][0]*x-axis[0][1]*y);
    weaponMaterial.uniform1i(weaponMaterial.style,weapon==9 ? (slot==5 ? 2 : 1) : 0);
    entry->lastDraw=weaponMaterial.frame;
    unsigned flag=1u<<(activeEye+2*(reloadRenderReady!=0));
    if(tracePoses && !(entry->logged&flag)){
        entry->logged|=flag;
        LOG_INFO("WEAPON MATERIAL weapon=%d eye=%d slot=%d split=%d smooth=%d",weapon,activeEye,slot,reloadRenderReady!=0,normals);
    }
    return 1;
}

static void EndWeaponMaterial(const WeaponMaterialDrawState *saved) {
    weaponMaterial.useProgram(saved->program);
    if(saved->customNormals){
        weaponMaterial.activeTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_BUFFER, saved->normalTexture);
    }
    weaponMaterial.activeTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, saved->texture);
    weaponMaterial.activeTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, saved->nativeTexture);
}
