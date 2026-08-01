# Mesa softpipe 构建探测结果（meson introspect）

> 探测配置：`-Dgallium-drivers=softpipe -Dglx=disabled -Dopengl=false -Dgles1=disabled -Dgles2=enabled -Degl=enabled -Dgbm=enabled -Dplatforms= -Dvulkan-drivers= -Dllvm=disabled`
>
> 探测目录 `/tmp/mesa_probe`（host native，仅为 dump 文件清单/命令，非交叉构建）。
> 源文件相对 `third_party/mesa/`。

## 文件清单

- `sources.md` — 每个 target 的源码文件 + link_with/link_whole/deps（从 `meson introspect --targets`）。
- `codegen.md` — 85 个 codegen 步骤（从 `build.ninja` 的 `CUSTOM_COMMAND` 规则），含命令行。
- 原 ninja：`/tmp/mesa_probe/build.ninja`（权威，可随时再抽）。

## 最终产物（5 个 .so）+ 链接图

从 `build.ninja` 的 `cpp_LINKER` 行提取（order-only `|` 后的是链接进去的静态库/外部库）：

### `libgallium-26.1.4.so`（megadriver DSO，EGL/gbm 运行时 dlopen 的就是它）
源：`src/gallium/targets/dri/dri_target.c`
链接静态库（全部 bundle 进 .so）：
```
libmesa_util_c11.a  libglcpp.a  libglsl.a  libcompiler.a  libnir.a  libvtn.a
libgallium.a  libgalliumvl.a  libpipe_loader_static.a  libsoftpipe.a  libdri.a
libswdri.a  libswkmsdri.a  libws_null.a  libwsw.a  libloader.a
libglapi.a  libmesa.a  libmesa_sse41.a  libblake3.a  libmesa_util.a
libmesa_util_clflush.a  libmesa_util_clflushopt.a  libmesa_util_simd.a  libxmlconfig.a
```
外部：libdrm.so libexpat libm libz

### `libEGL.so`
源：`src/egl/main/*.c` + `src/egl/drivers/dri2/{egl_dri2,platform_device,platform_surfaceless,platform_drm}.c` + 2 个 codegen（`g_egldispatchstubs.c/.h`，仅 glvnd 时实际为空——非 glvnd 仍生成）
链接：`libgallium-26.1.4.so`（DT_NEEDED）+ `libloader.a` + `libblake3/libmesa_util*/libxmlconfig.a` + libdrm/libexpat/libm/libz
→ EGL 不静态 bundle 驱动，运行时 dlopen `libgallium-26.1.4.so`。

### `libGLESv2.so`
源：`src/mesa/glapi/es2api/libgles2_public.c` + codegen `es2_glapi_mapi_tmp.h`
链接：`libgallium-26.1.4.so`（DT_NEEDED）+ libdrm/libm + `libmesa_util_c11.a`
→ 极薄 shim，所有符号转发到 megadriver。

### `libgbm.so`
源：`src/gbm/main/{backend,gbm}.c`
链接：`libloader.a` + util 系 + libdrm/libexpat/libm/libz
→ 不含驱动，运行时按 `gbm-backends-path` dlopen `dri_gbm.so`。

### `dri_gbm.so`（gbm 的 dri backend）
源：`src/gbm/backends/dri/gbm_dri.c`
链接：`libgallium-26.1.4.so`（DT_NEEDED）+ `libloader.a` + util 系

## 静态库依赖树（编译顺序）

```
softpipe      -> nir, mesautil
gallium       -> (gallium/auxiliary 全量) nir glsl glcpp mesa_util ...
mesa          -> glsl nir mesa_util glapi ...
glsl          -> glcpp, nir, builtin_types_h, glsl_parser(bison), glsl_lexer(flex)
glcpp         -> glcpp-lex(flex), glcpp-parse(bison)
nir           -> nir codegen(nir_opcodes/nir_intrinsics/nir_builder_opcodes...)
loader        -> mesa_util, xmlconfig
dri (st_dri) -> gallium, mesa, gallium aux
glapi         -> shared_glapi_mapi_tmp.h (mapi_abi.py)
pipe_loader_static, ws_null, wsw, swdri, swkmsdri  -> gallium aux
mesa_util(_c11/_simd/_clflush*/_clflushopt), blake3, isaspec, parson, xmlconfig, vtn, compiler, mesa_sse41
```

## codegen 翻译到 CMake

`codegen.md` 里两类命令：

1. **直接 python**：`python <script> <args> <out>`
   → `add_custom_command(OUTPUT <out> COMMAND ${PYTHON} <script> <args> <out>)`

2. **meson capture**：`meson --internal exe --capture <out> -- python <script> <args>`
   = 把 python 脚本 stdout 重定向到 `<out>`。
   → `add_custom_command(OUTPUT <out> COMMAND ${PYTHON} <script> <args> > <out>)`
   （`--internal exe --capture` 只是 meson 的 `sh -c '...'` 包装，去掉它直接重定向即可）

关键 codegen（必须复刻，否则编不过）：
- **glapi dispatch**：`es2_glapi_mapi_tmp.h`、`shared_glapi_mapi_tmp.h`、`glapi_mapi_tmp.h`、`glapi_gentable.c`（`mapi_abi.py`/`gen_gldispatch_mapi.py` + `gl_and_es_API.xml`/`registry/gl.xml`，依赖整个 `glapi/gen/*.xml` 集合）
- **EGL dispatch**：`g_egldispatchstubs.c/.h`（`gen_egl_dispatch.py` + `egl.xml`/`egl_other.xml`）
- **GLSL parser**：`glsl_parser.cpp/.h`（bison `glsl_parser.yy`）、`glsl_lexer.cpp`（flex `glsl_lexer.ll`）
- **glcpp**：`glcpp-lex.c`（flex）、`glcpp-parse.c/.h`（bison）
- **NIR**：`nir_opcodes.h/.c`、`nir_intrinsics.h`、`nir_intrinsics_indices.h`、`nir_builder_opcodes.h`、`nir_opt_algebraic`、`nir_constant_expressions.c`、`nir_intrinsic.c`（各自 python 脚本）
- **mesa API**：`api_exec_init.c`、`api_exec_decl.h`、`marshal_generated*.c/.h`、`get_hash.h`、`enums.c`、`dispatch.h`、`indirect*.c/.h`（`src/mesa/main/gen_marshal.py` 等）
- **format**：`u_format_gen.h`、`u_format_pack.h`、`u_format_table.c`、`format_srgb.c`、`format_info.h`（`u_format_table.py` + `u_format.yaml`）
- **杂项**：`git_sha1.h`、`driconf_static.h`、`shader_stats.h`、`builtin_types.h`、`u_tracepoints.c/.h`、`u_indices_gen.c`、`u_unfilled_gen.c`、`astc/bc1/bc4/float64/etc2/*_glsl.h`

全部 85 条详见 `codegen.md`。

## 注意

- 探测是 host native（gcc 13.3），用的是宿主 libexpat/libm/libz/libunwind/libzstd 等。**交叉构建时**这些要换成 sysroot：libdrm 已在 `build/sysroot`，libexpat/zlib 等需补（mesa `idep_xmlconfig` 强依赖 expat；zlib 被 driconf 等用）。
- 探测的 `EGL` deps 列表含 `valgrind/libunwind/libzstd`，这些是 meson 探测到的宿主可选依赖，交叉构建可关（`-Dvalgrind=disabled` 等）。
- LLVM：`LLVM Required: disabled` —— softpipe 路径不需要 LLVM，确认。
- `gbm` 的 `dri_gbm.so` 是运行时 backend；如果只想 EGL 不用 gbm，可省 `dri_gbm.so`，但 wlroots GLES renderer 走 gbm，建议留。

## 复跑探测

```bash
export PATH="/tmp/mesa_venv/bin:$PATH"
export PKG_CONFIG_PATH="/home/hesse/os-test/build/sysroot/usr/lib/pkgconfig"
meson setup /tmp/mesa_probe -Dgallium-drivers=softpipe -Dglx=disabled \
  -Dopengl=false -Dgles1=disabled -Dgles2=enabled -Degl=enabled -Dgbm=enabled \
  -Dplatforms= -Dvulkan-drivers= -Dllvm=disabled \
  -Dgallium-va=disabled -Dgallium-rusticl=false -Dvideo-codecs=
meson introspect /tmp/mesa_probe --targets > /tmp/mesa_targets.json
# build.ninja 在 /tmp/mesa_probe/build.ninja
```
