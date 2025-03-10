;; NOTE: Assertions have been generated by update_lit_checks.py --all-items and should not be edited.
;; NOTE: This test was ported using port_passes_tests_to_lit.py and could be cleaned up.

;; RUN: foreach %s %t wasm-opt -O3 --inline-functions-with-loops --flexible-inline-max-function-size=30 -S -o - | filecheck %s

(module
  ;; CHECK:      (type $t0 (func (param i32) (result i32)))
  (type $t0 (func (param i32) (result i32)))
  ;; CHECK:      (memory $memory 0)

  ;; CHECK:      (export "memory" (memory $memory))

  ;; CHECK:      (export "fib" (func $fib))

  ;; CHECK:      (export "looped" (func $looped))

  ;; CHECK:      (export "t0" (func $looped))

  ;; CHECK:      (export "t1" (func $t1))

  ;; CHECK:      (export "t2" (func $fib))

  ;; CHECK:      (export "t3" (func $t3))

  ;; CHECK:      (func $fib (; has Stack IR ;) (param $0 i32) (result i32)
  ;; CHECK-NEXT:  (if
  ;; CHECK-NEXT:   (i32.le_s
  ;; CHECK-NEXT:    (local.get $0)
  ;; CHECK-NEXT:    (i32.const 2)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:   (return
  ;; CHECK-NEXT:    (local.get $0)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (i32.add
  ;; CHECK-NEXT:   (call $fib
  ;; CHECK-NEXT:    (i32.sub
  ;; CHECK-NEXT:     (local.get $0)
  ;; CHECK-NEXT:     (i32.const 1)
  ;; CHECK-NEXT:    )
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:   (call $fib
  ;; CHECK-NEXT:    (i32.sub
  ;; CHECK-NEXT:     (local.get $0)
  ;; CHECK-NEXT:     (i32.const 2)
  ;; CHECK-NEXT:    )
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $fib (export "fib") (type $t0) (param $p0 i32) (result i32)
    (if $I0
      (i32.le_s
        (local.get $p0)
        (i32.const 2)
      )
      (then
        (return
          (local.get $p0)
        )
      )
    )
    (i32.add
      (call $fib
        (i32.sub
          (local.get $p0)
          (i32.const 1)
        )
      )
      (call $fib
        (i32.sub
          (local.get $p0)
          (i32.const 2)
        )
      )
    )
  )
  ;; CHECK:      (func $looped (; has Stack IR ;) (param $0 i32) (result i32)
  ;; CHECK-NEXT:  (loop $L0
  ;; CHECK-NEXT:   (if
  ;; CHECK-NEXT:    (i32.ge_s
  ;; CHECK-NEXT:     (local.get $0)
  ;; CHECK-NEXT:     (i32.const 0)
  ;; CHECK-NEXT:    )
  ;; CHECK-NEXT:    (block
  ;; CHECK-NEXT:     (local.set $0
  ;; CHECK-NEXT:      (i32.sub
  ;; CHECK-NEXT:       (local.get $0)
  ;; CHECK-NEXT:       (i32.const 1)
  ;; CHECK-NEXT:      )
  ;; CHECK-NEXT:     )
  ;; CHECK-NEXT:     (br $L0)
  ;; CHECK-NEXT:    )
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (local.get $0)
  ;; CHECK-NEXT: )
  (func $looped (export "looped") (type $t0) (param $p0 i32) (result i32)
    (loop $L0
      (if $I1
        (i32.ge_s
          (local.get $p0)
          (i32.const 0)
        )
        (then
          (local.set $p0
            (i32.sub
              (local.get $p0)
              (i32.const 1)
            )
          )
          (br $L0)
        )
      )
    )
    (local.get $p0)
  )

  (func $t0 (export "t0") (type $t0) (param $p0 i32) (result i32)
    (call $looped
      (local.get $p0)
    )
  )

  ;; CHECK:      (func $t1 (; has Stack IR ;) (param $0 i32) (result i32)
  ;; CHECK-NEXT:  (local.set $0
  ;; CHECK-NEXT:   (i32.add
  ;; CHECK-NEXT:    (local.get $0)
  ;; CHECK-NEXT:    (i32.const 1)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (loop $L0
  ;; CHECK-NEXT:   (if
  ;; CHECK-NEXT:    (i32.ge_s
  ;; CHECK-NEXT:     (local.get $0)
  ;; CHECK-NEXT:     (i32.const 0)
  ;; CHECK-NEXT:    )
  ;; CHECK-NEXT:    (block
  ;; CHECK-NEXT:     (local.set $0
  ;; CHECK-NEXT:      (i32.sub
  ;; CHECK-NEXT:       (local.get $0)
  ;; CHECK-NEXT:       (i32.const 1)
  ;; CHECK-NEXT:      )
  ;; CHECK-NEXT:     )
  ;; CHECK-NEXT:     (br $L0)
  ;; CHECK-NEXT:    )
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (local.get $0)
  ;; CHECK-NEXT: )
  (func $t1 (export "t1") (type $t0) (param $p0 i32) (result i32)
    (call $looped
      (i32.add
        (local.get $p0)
        (i32.const 1)
      )
    )
  )
  (func $t2 (export "t2") (type $t0) (param $p0 i32) (result i32)
    (call $fib
      (local.get $p0)
    )
  )

  ;; CHECK:      (func $t3 (; has Stack IR ;) (param $0 i32) (result i32)
  ;; CHECK-NEXT:  (call $fib
  ;; CHECK-NEXT:   (i32.add
  ;; CHECK-NEXT:    (local.get $0)
  ;; CHECK-NEXT:    (i32.const 1)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $t3 (export "t3") (type $t0) (param $p0 i32) (result i32)
    (call $fib
      (i32.add
        (local.get $p0)
        (i32.const 1)
      )
    )
  )
  (memory $memory (export "memory") 0)
)
