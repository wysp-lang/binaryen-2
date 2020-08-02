(module
 (export "convert-f32-u" (func $convert-f32-u))
 (export "convert-f64-u" (func $convert-f64-u))
 (export "convert-f32-s" (func $convert-f32-s))
 (export "convert-f64-s" (func $convert-f64-s))
 (func $convert-f32-u (result f32)
  (f32.convert_i64_u
   (i64.const 18446743523953737727)
  )
 )
 (func $convert-f64-u (result f64)
  (f64.convert_i64_u
   (i64.const 18446743523953737727)
  )
 )
 (func $convert-f32-s (result f32)
  (f32.convert_i64_s
   (i64.const 18446743523953737727)
  )
 )
 (func $convert-f64-s (result f64)
  (f64.convert_i64_s
   (i64.const 18446743523953737727)
  )
 )
)

