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
;;)
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
    ;; a second use, so it can't be removed after inlining
    (call $source)
  )
)
