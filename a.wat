;; BINARYEN_PASS_DEBUG=1 bin/wasm-opt a.wat -all  -O3 --inlining-optimizing
(module
 (global $global$0 (mut i32) (i32.const 10))
 (export "func_12_invoker" (func $1))
 (func $0 (param $0 i32) (param $1 i32) (result v128)
  (if
   (i32.eqz
    (global.get $global$0)
   )
   (return
    (v128.const i32x4 0x80007f0a 0x2000ffcd 0x0000ffc7 0xff93ffe4)
   )
  )
  (call $0
   (i16x8.extract_lane_u 4
    (block $label$2 (result v128)
     (if
      (global.get $global$0)
      (return
       (v128.const i32x4 0x000000ff 0x00000000 0x0000ffff 0x00000000)
      )
     )
     (v128.const i32x4 0xf0ab0000 0x00ff5730 0x67dbc4ff 0xdaffac99)
    )
   )
   (i32.const 0)
  )
 )
 (func $1
  (drop
   (call $0
    (i16x8.extract_lane_u 4
     (block $label$1 (result v128)
      (global.set $global$0
       (i32.sub
        (global.get $global$0)
        (i32.const 1)
       )
      )
      (v128.const i32x4 0xf0ab0000 0x00ff5730 0x67dbc4ff 0xdaffac99)
     )
    )
    (i32.const -89)
   )
  )
  (drop
   (call $0
    (i32.const 0)
    (i32.const -1756465363)
   )
  )
 )
)
