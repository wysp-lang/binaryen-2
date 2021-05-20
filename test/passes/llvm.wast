(module
 (global $global (mut i32) (i32.const 1))
 (start $startey)
 (func "foo" (result i32)
  (i32.add
   (i32.const 41)
   (i32.const 1)
  )
 )
 (func $startey
  (global.set $global
   (i32.const 2)
  )
 )
)
