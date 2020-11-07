(;;module
  (func $source1
    ;; too big for definite inlining; requires speculation
    (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop)
    (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop)
    (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop)
  )
  (func $target1
    (call $source1)
  )
)
(module
  (func $source
    (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop)
    (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop)
    (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop) (nop)
  )
  (func $target
    (call $source)
  )
  (func $targetb
    ;; a second use, so it can't be removed after inlining. but all the nops are
    ;; optimized away, so it turns out worth it.
    (call $source)
  )
)
(module
  (memory 10)
  (func $source
    ;; these stores are *not* optimized away, so we do not inline
    (i32.store offset=0 (i32.const 1) (i32.const 2))
    (i32.store offset=1 (i32.const 1) (i32.const 2))
    (i32.store offset=2 (i32.const 1) (i32.const 2))
    (i32.store offset=3 (i32.const 1) (i32.const 2))
    (i32.store offset=4 (i32.const 1) (i32.const 2))
    (i32.store offset=5 (i32.const 1) (i32.const 2))
    (i32.store offset=6 (i32.const 1) (i32.const 2))
    (i32.store offset=7 (i32.const 1) (i32.const 2))
    (i32.store offset=8 (i32.const 1) (i32.const 2))
    (i32.store offset=9 (i32.const 1) (i32.const 2))
  )
  (func $target
    (call $source)
  )
  (func $targetb
    (call $source)
  )
)
(module
  (memory 10)
  (func $source
    (i32.store offset=0 (i32.const 1) (i32.const 2))
    (i32.store offset=1 (i32.const 1) (i32.const 2))
    (i32.store offset=2 (i32.const 1) (i32.const 2))
    (i32.store offset=3 (i32.const 1) (i32.const 2))
    (i32.store offset=4 (i32.const 1) (i32.const 2))
    (i32.store offset=5 (i32.const 1) (i32.const 2))
    (i32.store offset=6 (i32.const 1) (i32.const 2))
    (i32.store offset=7 (i32.const 1) (i32.const 2))
    (i32.store offset=8 (i32.const 1) (i32.const 2))
    (i32.store offset=9 (i32.const 1) (i32.const 2))
  )
  (func $target
    ;; but with just one use, we can do it.
    (call $source)
  )
;;)
(module
  (memory 10)
  (func $source
    (i32.store offset=0 (i32.const 1) (i32.const 2))
    (i32.store offset=1 (i32.const 1) (i32.const 2))
    (i32.store offset=2 (i32.const 1) (i32.const 2))
    (i32.store offset=3 (i32.const 1) (i32.const 2))
    (i32.store offset=4 (i32.const 1) (i32.const 2))
    (i32.store offset=5 (i32.const 1) (i32.const 2))
    (i32.store offset=6 (i32.const 1) (i32.const 2))
    (i32.store offset=7 (i32.const 1) (i32.const 2))
    (i32.store offset=8 (i32.const 1) (i32.const 2))
    (i32.store offset=9 (i32.const 1) (i32.const 2))
  )
  (func $target
    ;; but with just one use, we can do it.
    (call $source)
  )
)
