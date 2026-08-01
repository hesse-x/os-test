## src/util/driconf_static.h
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/util/driconf_static.py ../../home/hesse/os-test/third_party/mesa/src/util/00-mesa-defaults.conf src/util/driconf_static.h`

## src/util/format_srgb.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/util/format_srgb.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/util/format_srgb.py`

## src/util/shader_stats.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/util/shader_stats.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/util/process_shader_stats.py ../../home/hesse/os-test/third_party/mesa/src/util/shader_stats.rnc ../../home/hesse/os-test/third_party/mesa/src/util/shader_stats.xml`

## src/util/format/u_format_gen.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/util/format/u_format_gen.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/util/format/u_format_table.py ../../home/hesse/os-test/third_party/mesa/src/util/format/u_format.yaml --enums`

## src/util/format/u_format_pack.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/util/format/u_format_pack.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/util/format/u_format_table.py ../../home/hesse/os-test/third_party/mesa/src/util/format/u_format.yaml --header`

## src/util/format/u_format_table.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/util/format/u_format_table.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/util/format/u_format_table.py ../../home/hesse/os-test/third_party/mesa/src/util/format/u_format.yaml`

## src/git_sha1.h
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/bin/git_sha1_gen.py --output src/git_sha1.h`

## src/compiler/builtin_types.h
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/compiler/builtin_types_h.py src/compiler/builtin_types.h`

## src/compiler/builtin_types.c
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/compiler/builtin_types_c.py src/compiler/builtin_types.c`

## src/compiler/ir_expression_operation.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/compiler/ir_expression_operation.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/compiler/glsl/ir_expression_operation.py enum`

## src/compiler/nir/nir_builder_opcodes.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/compiler/nir/nir_builder_opcodes.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/compiler/nir/nir_builder_opcodes_h.py`

## src/compiler/nir/nir_constant_expressions.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/compiler/nir/nir_constant_expressions.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/compiler/nir/nir_constant_expressions.py`

## src/compiler/nir/nir_opcodes.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/compiler/nir/nir_opcodes.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/compiler/nir/nir_opcodes_h.py`

## src/compiler/nir/nir_opcodes.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/compiler/nir/nir_opcodes.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/compiler/nir/nir_opcodes_c.py`

## src/compiler/nir/nir_opt_algebraic.c
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/compiler/nir/nir_opt_algebraic.py --out src/compiler/nir/nir_opt_algebraic.c`

## src/compiler/nir/nir_intrinsics.h
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/compiler/nir/nir_intrinsics_h.py --out src/compiler/nir/nir_intrinsics.h`

## src/compiler/nir/nir_intrinsics_indices.h
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/compiler/nir/nir_intrinsics_indices_h.py --out src/compiler/nir/nir_intrinsics_indices.h`

## src/compiler/nir/nir_intrinsics.c
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/compiler/nir/nir_intrinsics_c.py --out src/compiler/nir/nir_intrinsics.c`

## src/compiler/spirv/vtn_gather_types.c
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/compiler/spirv/vtn_gather_types_c.py ../../home/hesse/os-test/third_party/mesa/src/compiler/spirv/spirv.core.grammar.json src/compiler/spirv/vtn_gather_types.c`

## src/compiler/spirv/spirv_info.h src/compiler/spirv/spirv_info.c
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/compiler/spirv/spirv_info_gen.py --json ../../home/hesse/os-test/third_party/mesa/src/compiler/spirv/spirv.core.grammar.json --out-h src/compiler/spirv/spirv_info.h --out-c src/compiler/spirv/spirv_info.c`

## src/compiler/spirv/vtn_generator_ids.h
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/compiler/spirv/vtn_generator_ids_h.py ../../home/hesse/os-test/third_party/mesa/src/compiler/spirv/spir-v.xml src/compiler/spirv/vtn_generator_ids.h`

## src/compiler/glsl/glcpp/glcpp-parse.c src/compiler/glsl/glcpp/glcpp-parse.h
- cmd: `/usr/bin/bison -Wno-deprecated -o src/compiler/glsl/glcpp/glcpp-parse.c -p glcpp_parser_ --defines=src/compiler/glsl/glcpp/glcpp-parse.h ../../home/hesse/os-test/third_party/mesa/src/compiler/glsl/glcpp/glcpp-parse.y`

## src/compiler/glsl/glcpp/glcpp-lex.c
- cmd: `/usr/bin/flex -o src/compiler/glsl/glcpp/glcpp-lex.c ../../home/hesse/os-test/third_party/mesa/src/compiler/glsl/glcpp/glcpp-lex.l`

## src/compiler/glsl/glsl_parser.cpp src/compiler/glsl/glsl_parser.h
- cmd: `/usr/bin/bison -Wno-deprecated -o src/compiler/glsl/glsl_parser.cpp -p _mesa_glsl_ --defines=src/compiler/glsl/glsl_parser.h ../../home/hesse/os-test/third_party/mesa/src/compiler/glsl/glsl_parser.yy`

## src/compiler/glsl/glsl_lexer.cpp
- cmd: `/usr/bin/flex -o src/compiler/glsl/glsl_lexer.cpp ../../home/hesse/os-test/third_party/mesa/src/compiler/glsl/glsl_lexer.ll`

## src/compiler/glsl/ir_expression_operation_constant.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/compiler/glsl/ir_expression_operation_constant.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/compiler/glsl/ir_expression_operation.py constant`

## src/compiler/glsl/ir_expression_operation_strings.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/compiler/glsl/ir_expression_operation_strings.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/compiler/glsl/ir_expression_operation.py strings`

## src/compiler/glsl/float64_glsl.h
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/util/xxd.py ../../home/hesse/os-test/third_party/mesa/src/compiler/glsl/float64.glsl src/compiler/glsl/float64_glsl.h -n float64_source`

## src/compiler/glsl/cross_platform_settings_piece_all.h
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/util/xxd.py ../../home/hesse/os-test/third_party/mesa/src/compiler/glsl/CrossPlatformSettings_piece_all.glsl src/compiler/glsl/cross_platform_settings_piece_all.h -n cross_platform_settings_piece_all_header`

## src/compiler/glsl/bc1_glsl.h
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/util/xxd.py ../../home/hesse/os-test/third_party/mesa/src/compiler/glsl/bc1.glsl src/compiler/glsl/bc1_glsl.h -n bc1_source`

## src/compiler/glsl/bc4_glsl.h
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/util/xxd.py ../../home/hesse/os-test/third_party/mesa/src/compiler/glsl/bc4.glsl src/compiler/glsl/bc4_glsl.h -n bc4_source`

## src/compiler/glsl/etc2_rgba_stitch_glsl.h
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/util/xxd.py ../../home/hesse/os-test/third_party/mesa/src/compiler/glsl/etc2_rgba_stitch.glsl src/compiler/glsl/etc2_rgba_stitch_glsl.h -n etc2_rgba_stitch_source`

## src/compiler/glsl/astc_glsl.h
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/util/xxd.py ../../home/hesse/os-test/third_party/mesa/src/compiler/glsl/astc_decoder.glsl src/compiler/glsl/astc_glsl.h -n astc_source`

## src/mesa/glapi/glapi/gen/glapi_mapi_tmp.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/glapi_mapi_tmp.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/../../mapi_abi.py --printer glapi --gl_symbols ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/../../../../glx/libgl-symbols.txt ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml`

## src/mesa/glapi/glapi/gen/glapi_gentable.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/glapi_gentable.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_gentable.py -f ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml`

## src/mesa/glapi/glapi/gen/enums.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/enums.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_enums.py -f ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/../registry/gl.xml`

## src/mesa/glapi/glapi/gen/api_exec_init.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/api_exec_init.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/api_exec_init.py -f ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml`

## src/mesa/glapi/glapi/gen/api_exec_decl.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/api_exec_decl.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/api_exec_decl_h.py -f ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml`

## src/mesa/glapi/glapi/gen/api_save_init.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/api_save_init.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/api_save_init_h.py -f ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml`

## src/mesa/glapi/glapi/gen/api_save.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/api_save.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/api_save_h.py -f ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml`

## src/mesa/glapi/glapi/gen/api_beginend_init.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/api_beginend_init.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/api_beginend_init_h.py -f ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml`

## src/mesa/glapi/glapi/gen/api_hw_select_init.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/api_hw_select_init.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/api_hw_select_init_h.py -f ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_API.xml`

## src/mesa/glapi/glapi/gen/dispatch.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/dispatch.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_table.py -f ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml -m dispatch`

## src/mesa/glapi/glapi/gen/marshal_generated.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/marshal_generated.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/marshal_generated_h.py ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml 8`

## src/mesa/glapi/glapi/gen/unmarshal_table.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/unmarshal_table.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/unmarshal_table_c.py ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml 8`

## src/mesa/glapi/glapi/gen/marshal_generated0.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/marshal_generated0.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/marshal_generated_c.py ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml 0 8 8`

## src/mesa/glapi/glapi/gen/marshal_generated1.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/marshal_generated1.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/marshal_generated_c.py ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml 1 8 8`

## src/mesa/glapi/glapi/gen/marshal_generated2.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/marshal_generated2.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/marshal_generated_c.py ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml 2 8 8`

## src/mesa/glapi/glapi/gen/marshal_generated3.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/marshal_generated3.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/marshal_generated_c.py ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml 3 8 8`

## src/mesa/glapi/glapi/gen/marshal_generated4.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/marshal_generated4.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/marshal_generated_c.py ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml 4 8 8`

## src/mesa/glapi/glapi/gen/marshal_generated5.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/marshal_generated5.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/marshal_generated_c.py ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml 5 8 8`

## src/mesa/glapi/glapi/gen/marshal_generated6.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/marshal_generated6.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/marshal_generated_c.py ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml 6 8 8`

## src/mesa/glapi/glapi/gen/marshal_generated7.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/marshal_generated7.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/marshal_generated_c.py ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml 7 8 8`

## src/mesa/glapi/glapi/gen/indirect.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/indirect.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/glX_proto_send.py -f ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_API.xml -m proto -s ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/../../../../glx/libgl-symbols.txt`

## src/mesa/glapi/glapi/gen/indirect.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/indirect.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/glX_proto_send.py -f ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_API.xml -m init_h -s ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/../../../../glx/libgl-symbols.txt`

## src/mesa/glapi/glapi/gen/indirect_init.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/indirect_init.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/glX_proto_send.py -f ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_API.xml -m init_c -s ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/../../../../glx/libgl-symbols.txt`

## src/mesa/glapi/glapi/gen/indirect_size.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/indirect_size.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/glX_proto_size.py -f ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_API.xml --only-set -m size_h --header-tag _INDIRECT_SIZE_H_`

## src/mesa/glapi/glapi/gen/indirect_size.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/glapi/gen/indirect_size.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/glX_proto_size.py -f ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_API.xml --only-set -m size_c`

## src/mesa/glapi/shared-glapi/shared_glapi_mapi_tmp.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/shared-glapi/shared_glapi_mapi_tmp.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/shared-glapi/../mapi_abi.py --printer shared-glapi --gl_symbols ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/shared-glapi/../../../glx/libgl-symbols.txt ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/shared-glapi/../glapi/gen/gl_and_es_API.xml`

## src/mesa/program/lex.yy.c
- cmd: `/usr/bin/flex -o src/mesa/program/lex.yy.c ../../home/hesse/os-test/third_party/mesa/src/mesa/program/program_lexer.l`

## src/mesa/program/program_parse.tab.c src/mesa/program/program_parse.tab.h
- cmd: `/usr/bin/bison -Wno-deprecated -o src/mesa/program/program_parse.tab.c --defines=src/mesa/program/program_parse.tab.h ../../home/hesse/os-test/third_party/mesa/src/mesa/program/program_parse.y`

## src/mesa/format_fallback.c
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/main/format_fallback.py ../../home/hesse/os-test/third_party/mesa/src/mesa/main/formats.csv src/mesa/format_fallback.c`

## src/mesa/get_hash.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/get_hash.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/main/get_hash_generator.py -f ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/glapi/gen/gl_and_es_API.xml`

## src/mesa/format_info.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/format_info.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/main/format_info.py ../../home/hesse/os-test/third_party/mesa/src/mesa/main/formats.csv`

## src/gallium/auxiliary/draw_nir_lower_opcodes.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/gallium/auxiliary/draw_nir_lower_opcodes.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/gallium/auxiliary/draw/draw_nir_lower_opcodes.py -p /home/hesse/os-test/third_party/mesa/src/compiler/nir/`

## src/gallium/auxiliary/tr_util.c src/gallium/auxiliary/tr_util.h
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/gallium/auxiliary/driver_trace/enums2names.py ../../home/hesse/os-test/third_party/mesa/src/gallium/auxiliary/../include/pipe/p_defines.h ../../home/hesse/os-test/third_party/mesa/src/gallium/auxiliary/../include/pipe/p_video_enums.h ../../home/hesse/os-test/third_party/mesa/src/gallium/auxiliary/../../util/blend.h -C src/gallium/auxiliary/tr_util.c -H src/gallium/auxiliary/tr_util.h`

## src/gallium/auxiliary/u_tracepoints.c
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/gallium/auxiliary/util/u_tracepoints.py -p /home/hesse/os-test/third_party/mesa/src/util/perf/ -C src/gallium/auxiliary/u_tracepoints.c`

## src/gallium/auxiliary/u_tracepoints.h
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/gallium/auxiliary/util/u_tracepoints.py -p /home/hesse/os-test/third_party/mesa/src/util/perf/ -H src/gallium/auxiliary/u_tracepoints.h`

## src/gallium/auxiliary/u_indices_gen.c
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/gallium/auxiliary/indices/u_indices_gen.py src/gallium/auxiliary/u_indices_gen.c`

## src/gallium/auxiliary/u_unfilled_gen.c
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/gallium/auxiliary/indices/u_unfilled_gen.py src/gallium/auxiliary/u_unfilled_gen.c`

## src/mesa/glapi/es2api/es2_glapi_mapi_tmp.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/mesa/glapi/es2api/es2_glapi_mapi_tmp.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/es2api/../new/gen_gldispatch_mapi.py glesv2 ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/es2api/../glapi/registry/gl.xml`

## src/mesa/glapi/es2api/gles2.def
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/bin/gen_vs_module_defs.py --in_file ../../home/hesse/os-test/third_party/mesa/src/mesa/glapi/es2api/gles2.def.in --out_file src/mesa/glapi/es2api/gles2.def --compiler_abi gcc --compiler_id gcc --cpu_family x86_64`

## src/egl/g_egldispatchstubs.c
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/egl/g_egldispatchstubs.c -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/egl/generate/gen_egl_dispatch.py source ../../home/hesse/os-test/third_party/mesa/src/egl/generate/egl.xml ../../home/hesse/os-test/third_party/mesa/src/egl/generate/egl_other.xml`

## src/egl/g_egldispatchstubs.h
- cmd: `/tmp/mesa_venv/bin/meson --internal exe --capture src/egl/g_egldispatchstubs.h -- /tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/src/egl/generate/gen_egl_dispatch.py header ../../home/hesse/os-test/third_party/mesa/src/egl/generate/egl.xml ../../home/hesse/os-test/third_party/mesa/src/egl/generate/egl_other.xml`

## src/egl/egl.def
- cmd: `/tmp/mesa_venv/bin/python3.12 ../../home/hesse/os-test/third_party/mesa/bin/gen_vs_module_defs.py --in_file ../../home/hesse/os-test/third_party/mesa/src/egl/main/egl.def.in --out_file src/egl/egl.def --compiler_abi gcc --compiler_id gcc --cpu_family x86_64`

## meson-internal__test
- cmd: `/tmp/mesa_venv/bin/meson test --no-rebuild --print-errorlogs`

## meson-internal__benchmark
- cmd: `/tmp/mesa_venv/bin/meson test --benchmark --logbase benchmarklog --num-processes=1 --no-rebuild`

## meson-internal__install
- cmd: `/tmp/mesa_venv/bin/meson install --no-rebuild`

## meson-internal__dist
- cmd: `/tmp/mesa_venv/bin/meson dist`

## meson-internal__scan-build
- cmd: `/tmp/mesa_venv/bin/meson --internal scanbuild /home/hesse/os-test/third_party/mesa /tmp/mesa_probe subprojects /tmp/mesa_venv/bin/meson setup -D:allow-broken-lto=false '-D:allow-fallback-for=['"'"'perfetto'"'"']' -D:allow-kcmp=auto -D:amd-use-llvm=true -D:amdgpu-virtio=false -D:android-libbacktrace=auto -D:android-libperfetto=auto -D:android-strict=true -D:android-stub=false -D:build-aco-tests=false -D:build-radv-tests=false -D:build-tests=false -D:custom-shader-replacement= -D:d3d-drivers-path= '-D:datasources=['"'"'auto'"'"']' -D:display-info=auto -D:draw-use-llvm=true -D:dri-drivers-path= -D:egl-lib-suffix= -D:egl-native-platform=auto -D:egl=enabled -D:enable-glcpp-tests=true -D:execmem=true -D:expat=auto '-D:freedreno-kmds=['"'"'msm'"'"']' -D:gallium-d3d10-dll-name=libgallium_d3d10 -D:gallium-d3d10umd=false -D:gallium-d3d12-graphics=auto -D:gallium-d3d12-video=auto '-D:gallium-drivers=['"'"'softpipe'"'"']' -D:gallium-extra-hud=false -D:gallium-mediafoundation-test=false -D:gallium-mediafoundation=auto '-D:gallium-rusticl-enable-drivers=['"'"'auto'"'"']' -D:gallium-rusticl=false -D:gallium-va=disabled -D:gallium-wgl-dll-name=libgallium_wgl -D:gbm-backends-path= -D:gbm=enabled -D:gles-lib-suffix= -D:gles1=disabled -D:gles2=enabled -D:glvnd-vendor-name=mesa -D:glvnd=auto -D:glx-direct=true -D:glx-read-only-text=false -D:glx=disabled -D:gpuvis=false -D:html-docs-path= -D:html-docs=disabled -D:imagination-srv=false '-D:imagination-uscgen-devices=[]' -D:install-intel-gpu-tests=false -D:install-mesa-clc=false -D:install-precomp-compiler=false -D:intel-elk=true -D:intel-rt=auto -D:intel-virtio-experimental=false '-D:legacy-wayland=[]' '-D:legacy-x11=[]' -D:libgbm-external=false -D:libunwind=auto -D:llvm-orcjit=false -D:llvm=disabled -D:lmsensors=auto '-D:mediafoundation-codecs=['"'"'all'"'"']' -D:mediafoundation-store-dll=false -D:mesa-clc-bundle-headers=auto -D:mesa-clc=auto -D:microsoft-clc=auto -D:min-windows-version=8 -D:moltenvk-dir= -D:opengl=false -D:perfetto=false -D:platform-sdk-version=35 '-D:platforms=[]' -D:power8=auto -D:precomp-compiler=auto -D:radeonsi-build-id= -D:radv-build-id= -D:selinux=true -D:shader-cache-default=true -D:shader-cache-max-size= -D:shader-cache=auto -D:shared-glapi=auto -D:shared-llvm=auto -D:spirv-to-dxil=false -D:spirv-tools=auto -D:split-debug=disabled -D:sse2=true '-D:static-libclc=[]' -D:sysprof=false -D:teflon=false '-D:tools=[]' -D:unversion-libgallium=false -D:va-libs-path= -D:valgrind=auto '-D:video-codecs=[]' -D:virtgpu_kumquat=false -D:vmware-mks-stats=false -D:vulkan-beta=false '-D:vulkan-drivers=[]' -D:vulkan-icd-dir= '-D:vulkan-layers=[]' -D:vulkan-manifest-per-architecture=true -D:xlib-lease=auto -D:xmlconfig=auto -D:zlib=enabled -D:zstd=auto`

## meson-internal__clang-format
- cmd: `/tmp/mesa_venv/bin/meson --internal clangformat /home/hesse/os-test/third_party/mesa /tmp/mesa_probe --color always`

## meson-internal__clang-format-check
- cmd: `/tmp/mesa_venv/bin/meson --internal clangformat /home/hesse/os-test/third_party/mesa /tmp/mesa_probe --check --color always`

## meson-internal__uninstall
- cmd: `/tmp/mesa_venv/bin/meson --internal uninstall`

## meson-internal__clean-ctlist
- cmd: `/tmp/mesa_venv/bin/meson --internal cleantrees /tmp/mesa_probe/meson-private/cleantrees.dat`

## meson-internal__clean
- cmd: `/usr/bin/ninja -t clean`

