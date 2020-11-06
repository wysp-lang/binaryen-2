(module
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
