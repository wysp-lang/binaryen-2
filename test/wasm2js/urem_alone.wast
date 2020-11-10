(module
  (func (export "rem_u") (param $x i64) (param $y i64) (result i64)
    ;; this will include i64 support code. this test ensures that that code
    ;; works ok when this is all by itself - without bringing in lots of other
    ;; i64 code as well (which is common)
    (i64.rem_u (local.get $x) (local.get $y))
  )
)
