(module
  (export "yes" (func $yes))
  (export "no-loops-but-one-use-but-exported" (func $no-loops-but-one-use-but-exported))
  (table 1 1 funcref)
  (elem (i32.const 0) $no-loops-but-one-use-but-tabled)

  (func $yes (result i32) ;; inlinable: small, lightweight, even with multi uses and a global use, ok when opt-level=3
    (i32.const 1)
  )
  (func $yes-big-but-single-use (result i32)
    (nop) (nop) (nop) (nop) (nop) (nop)
    (nop) (nop) (nop) (nop) (nop) (nop)
    (nop) (nop) (nop) (nop) (nop) (nop)
    (nop) (nop) (nop) (nop) (nop) (nop)
    (nop) (nop) (nop) (nop) (nop) (nop)
    (nop) (nop) (nop) (nop) (nop) (nop)
    (i32.const 1)
  )
  (func $no-calls (result i32)
    (call $yes)
  )
  (func $yes-calls-but-one-use (result i32)
    (call $yes)
  )
  (func $no-loops (result i32)
    (loop (result i32)
      (i32.const 1)
    )
  )
  (func $yes-loops-but-one-use (result i32)
    (loop (result i32)
      (i32.const 1)
    )
  )
  (func $no-loops-but-one-use-but-exported (result i32)
    (loop (result i32)
      (i32.const 1)
    )
  )
  (func $no-loops-but-one-use-but-tabled (result i32)
    (loop (result i32)
      (i32.const 1)
    )
  )
  (func $intoHere
    (drop (call $yes))
    (drop (call $yes-big-but-single-use))
    (drop (call $no-calls))
    (drop (call $no-calls))
    (drop (call $yes-calls-but-one-use))
    (drop (call $no-loops))
    (drop (call $no-loops))
    (drop (call $yes-loops-but-one-use))
    (drop (call $no-loops-but-one-use-but-exported))
    (drop (call $no-loops-but-one-use-but-tabled))
  )
)
;; Fuzz testcase for a name collision between the parameter to an inlined call
;; and the new block for the inlined contents.
(module
 (func $0 (param $x i32) (result i32)
  (i32.const 2)
 )
 (func $1
  ;; a block name from a previous inlining
  (block $__inlined_func$0
   (drop
    (call $0
     (block (result i32)
      (br $__inlined_func$0) ;; this goes to the outer block here. after we
                             ;; inline, we will create another block with a
                             ;; similar name, and must not be confused.
      (i32.const 1)
     )
    )
   )
  )
 )
)
