
#ifdef ANDROID
#include <android/log.h>
#define RAYGL_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "Rayverse-GL", __VA_ARGS__)
#endif

typedef struct basic_shader_t {
    u32 program;
    s32 u_texture0;
    s32 attrib_location_pos;
    s32 attrib_location_tex_coord;
} basic_shader_t;

basic_shader_t basic_shader;
static u32 vbo_screen;
static u32 vao_screen;

#ifdef ANDROID
// GLES 2.0 shaders (SDL creates a GLES 2.0 context on Android). The surface
// memory is BGRA byte order (see render.c), so the fragment shader swizzles
// .bgra on read to compensate for the GL_RGBA upload in opengl_upload_surface.
static char vertex_shader_source[] =
        "#version 100\n"
        "attribute vec2 pos;\n"
        "attribute vec2 tex_coord;\n"
        "varying vec2 v_tex_coord;\n"
        "void main() {\n"
        "    gl_Position = vec4(pos.x, -pos.y, 0.0, 1.0);\n"
        "    v_tex_coord = tex_coord;\n"
        "}";

static char fragment_shader_source[] =
        "#version 100\n"
        "precision mediump float;\n"
        "varying vec2 v_tex_coord;\n"
        "uniform sampler2D texture0;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(texture0, v_tex_coord).bgra;\n"
        "}";
#else
static char vertex_shader_source[] =
        "#version 330 core\n"
        "\n"
        "layout (location = 0) in vec2 pos;\n"
        "layout (location = 1) in vec2 tex_coord;\n"
        "\n"
        "out VS_OUT {\n"
        "    vec2 tex_coord;\n"
        "} vs_out;\n"
        "\n"
        "void main() {\n"
        "    gl_Position = vec4(pos.x, -pos.y, 0.0f, 1.0f);\n"
        "    vs_out.tex_coord = tex_coord;\n"
        "}";

static char fragment_shader_source[] =
        "#version 330 core\n"
        "\n"
        "in VS_OUT {\n"
        "    vec2 tex_coord;\n"
        "} fs_in;\n"
        "\n"
        "uniform sampler2D texture0;\n"
        "uniform float t;\n"
        "\n"
        "out vec4 fragColor;\n"
        "\n"
        "void main() {\n"
        "    fragColor = texture(texture0, fs_in.tex_coord);\n"
        "}";
#endif

void load_shader(u32 shader, const char* shader_source) {
    const char* sources[] = { shader_source, };
    glShaderSource(shader, COUNT(sources), sources, NULL);
    glCompileShader(shader);

    s32 success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success) {
//		printf("Loaded %sshader: %s\n", source_from_file ? "" : "cached ", source_filename);
    } else {
        char info_log[2048];
        glGetShaderInfoLog(shader, sizeof(info_log), NULL, info_log);
        printf("Error: compilation of shader %d failed:\n%s", shader, info_log);
        printf("Shader source: %s\n", shader_source);
#ifdef ANDROID
        RAYGL_LOGI("shader %d compile FAILED: %s", shader, info_log);
#endif
    }
}

u32 load_basic_shader_program(const char* vert_source, const char* frag_source) {
    u32 vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    u32 fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);

    load_shader(vertex_shader, vert_source);
    load_shader(fragment_shader, frag_source);

    u32 shader_program = glCreateProgram();

    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);
    glLinkProgram(shader_program);

    {
        s32 success;
        glGetProgramiv(shader_program, GL_LINK_STATUS, &success);
        if (!success) {
            char info_log[2048];
            glGetProgramInfoLog(shader_program, sizeof(info_log), NULL, info_log);
            printf("Error: shader linking failed: %s", info_log);
#ifdef ANDROID
            RAYGL_LOGI("shader LINK FAILED: %s", info_log);
#endif
            fatal_error();
        }
    }

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    return shader_program;
}

s32 get_attrib(u32 program, const char *name) {
    s32 attribute = glGetAttribLocation(program, name);
    if (attribute == -1)
        printf("Could not get attribute location %s\n", name);
    return attribute;
}

s32 get_uniform(u32 program, const char *name) {
    s32 uniform = glGetUniformLocation(program, name);
    if (uniform == -1)
        printf("Could not get uniform location %s\n", name);
    return uniform;
}

void init_draw_normalized_quad() {
    static bool initialized = false;
    ASSERT(!initialized);
    initialized = true;

#ifdef ANDROID
    // GLES: no VAO support, use default VBO binding
#else
    glGenVertexArrays(1, &vao_screen);
    glBindVertexArray(vao_screen);
#endif

    glGenBuffers(1, &vbo_screen);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_screen);

    static float vertices[] = {
            -1.0f, -1.0f, 0.0f, 0.0f, // x, y, (z = 0,)  u, v
            +1.0f, -1.0f, 1.0f, 0.0f,
            -1.0f, +1.0f, 0.0f, 1.0f,

            -1.0f, +1.0f, 0.0f, 1.0f,
            +1.0f, -1.0f, 1.0f, 0.0f,
            +1.0f, +1.0f, 1.0f, 1.0f,
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    GLsizei vertex_stride = 4 * sizeof(float);
    glVertexAttribPointer(basic_shader.attrib_location_pos, 2, GL_FLOAT, GL_FALSE, vertex_stride, (void*)0); // position coordinates
    glEnableVertexAttribArray(basic_shader.attrib_location_pos);
    glVertexAttribPointer(basic_shader.attrib_location_tex_coord, 2, GL_FLOAT, GL_FALSE, vertex_stride, (void*)(2*sizeof(float))); // texture coordinates
    glEnableVertexAttribArray(basic_shader.attrib_location_tex_coord);
}

bool linux_init_opengl(app_state_t* app_state) {

    SDL_GLContext gl_context = SDL_GL_CreateContext(app_state->sdl.window);
#ifdef ANDROID
    if (!gl_context) {
        RAYGL_LOGI("SDL_GL_CreateContext FAILED: %s", SDL_GetError());
    }
#endif
    SDL_GL_MakeCurrent(app_state->sdl.window, gl_context);

#ifdef ANDROID
    {
        const char* gl_ver = (const char*)glGetString(GL_VERSION);
        const char* gl_ren = (const char*)glGetString(GL_RENDERER);
        const char* glsl   = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
        RAYGL_LOGI("GL_VERSION=%s RENDERER=%s GLSL=%s",
            gl_ver?gl_ver:"?", gl_ren?gl_ren:"?", glsl?glsl:"?");
    }
#endif

//    char* version_string = (char*)glGetString(GL_VERSION);
//    printf("OpenGL supported version: %s\n", version_string);

    SDL_GL_SetSwapInterval(1); // Enable vsync
    app_state->vsync_enabled = true;

#if defined(ANDROID) || defined(__APPLE__)
    // Android: no GLEW needed; Apple: not used either
#else
    bool err = glewInit() != GLEW_OK;
    if (err) {
        fprintf(stderr, "Failed to initialize OpenGL loader!\n");
        fatal_error();
    }
#endif
    // load the shader that renders the surface to the screen
    basic_shader.program = load_basic_shader_program(vertex_shader_source, fragment_shader_source);
    basic_shader.u_texture0 = get_uniform(basic_shader.program, "texture0");
    basic_shader.attrib_location_pos = get_attrib(basic_shader.program, "pos");
    basic_shader.attrib_location_tex_coord = get_attrib(basic_shader.program, "tex_coord");
#ifdef ANDROID
    RAYGL_LOGI("shader: program=%u pos=%d tex_coord=%d tex0=%d",
        basic_shader.program, basic_shader.attrib_location_pos,
        basic_shader.attrib_location_tex_coord, basic_shader.u_texture0);
#endif

    glUseProgram(basic_shader.program);
    glUniform1i(basic_shader.u_texture0, 0);

    init_draw_normalized_quad();
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &app_state->opengl.max_texture_size);

	glEnable(GL_TEXTURE_2D);
	glGenTextures(1, &app_state->opengl.screen_texture);
	glBindTexture(GL_TEXTURE_2D, app_state->opengl.screen_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);



	return true;
}

void surface_clear(surface_t* surface) {
	memset(surface->memory, 0, surface->memory_size);
}

void surface_resize(surface_t* surface, int width, int height) {
	surface->width = width;
	surface->height = height;
	surface->bytes_per_pixel = 4;
    // NOTE: on Windows, we use OpenGL textures with a power-of-2 size here. This is for compatibility with old GPUs.
    // (On Windows, you might want to run on old GPUs paired with Win9x.)
    // However, on platforms other than Windows there is not really a need to deal with this restriction.
    surface->width_pow2 = width;
    surface->height_pow2 = height;
	surface->pitch = surface->width * surface->bytes_per_pixel;
	u32 memory_needed = surface->width * surface->height * surface->bytes_per_pixel;
	if (memory_needed > surface->memory_size) {
		u8* new_ptr = (u8*) realloc(surface->memory, memory_needed);
		if (!new_ptr) {
			fatal_error();
		}
		surface->memory = new_ptr;
		surface->memory_size = memory_needed;
	}
	surface_clear(surface);
}

void opengl_upload_surface(app_state_t* app_state, surface_t* surface, int client_width, int client_height) {
#ifdef ANDROID
    static bool logged_once = false;
    if (!logged_once) {
        logged_once = true;
        RAYGL_LOGI("upload: client=%dx%d surf=%dx%d pow2=%dx%d glErr=0x%x",
            client_width, client_height, surface->width, surface->height,
            surface->width_pow2, surface->height_pow2, glGetError());
    }
#endif
#ifndef ANDROID
	glDrawBuffer(GL_BACK);
#endif
#ifdef ANDROID
	if (client_height > client_width) {
		/* Portrait: letterbox to preserve the 320x200 aspect ratio and anchor
		 * the picture to the top, leaving the space below free for the touch
		 * controls (GL viewport y is measured from the bottom). */
		int vp_h = client_width * 200 / 320;
		glViewport(0, client_height - vp_h, client_width, vp_h);
	} else {
		glViewport(0, 0, client_width, client_height);
	}
#else
	glViewport(0, 0, client_width, client_height);
#endif

    glBindTexture(GL_TEXTURE_2D, app_state->opengl.screen_texture);
#ifdef ANDROID
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surface->width_pow2, surface->height_pow2, 0, GL_RGBA, GL_UNSIGNED_BYTE, surface->memory);
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surface->width_pow2, surface->height_pow2, 0, GL_BGRA, GL_UNSIGNED_BYTE, surface->memory);
#endif

    glUseProgram(basic_shader.program);
#ifdef ANDROID
    // GLES: no VAO, just use VBO directly
#else
    glBindVertexArray(vao_screen);
#endif
    glDisable(GL_DEPTH_TEST); // because we want to make sure the quad always renders in front of everything else
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, app_state->opengl.screen_texture);
    glDrawArrays(GL_TRIANGLES, 0, 6);
#ifdef ANDROID
    // GLES: no VAO unbind needed
#else
    glBindVertexArray(0);
#endif
}
