;; NOTE: Assertions have been generated by update_lit_checks.py --all-items and should not be edited.
;; NOTE: This test was ported using port_passes_tests_to_lit.py and could be cleaned up.

;; RUN: foreach %s %t wasm-opt --flatten --simplify-locals-nonesting --dfo -O3 -S -o - | filecheck %s

(module
 (memory 1)
 (func "if-select"
  (local $var$0 i32)
  (nop)
  (drop
   (if (result i32)
    (select
     (i32.const 65473)
     (i32.const 1)
     (local.get $var$0)
    )
    (i32.const -2405046)
    (i32.const 1)
   )
  )
 )
 (func "unreachable-body-update-zext" (result f64)
  (if
   (i32.eqz
    (i32.const 0)
   )
   (unreachable)
  )
  (f64.const -9223372036854775808)
 )
 (func "ssa-const" (param $var$0 i32) (param $var$1 f64) (param $var$2 f64) (result i32)
  (block $label$1 (result i32)
   (block $label$2
    (if
     (i32.const 1)
     (block
      (drop
       (loop $label$5 (result i64)
        (if (result i64)
         (i32.const 0)
         (i64.load offset=22
          (i32.and
           (br_if $label$1
            (i32.const 0)
            (i32.const 0)
           )
           (i32.const 15)
          )
          (i64.const 1)
         )
         (i64.const 1)
        )
       )
      )
     )
    )
    (unreachable)
   )
  )
 )
 (func "if-nothing" (param $var$0 i64)
  (local $var$1 i32)
  (local $var$2 i32)
  (block $label$1
   (loop $label$2
    (block $label$3
     (block $label$4
      (br_if $label$3
       (i32.eqz
        (if (result i32)
         (i32.const 0)
         (i32.const 0)
         (local.get $var$2)
        )
       )
      )
      (unreachable)
     )
     (unreachable)
    )
   )
   (unreachable)
  )
 )
 (func "only-dfo" (param $var$0 f64) (result i32)
  (local $var$1 i32)
  (local $var$2 i32)
  (local $var$3 i32)
  (local $var$4 i32)
  (loop $label$1
   (local.set $var$3
    (local.tee $var$1
     (local.tee $var$2
      (local.get $var$1)
     )
    )
   )
   (if
    (i32.eqz
     (local.get $var$4)
    )
    (block
     (local.set $var$4
      (select
       (local.get $var$3)
       (i32.const -2147483648)
       (local.get $var$2)
      )
     )
     (br $label$1)
    )
   )
  )
  (i32.const -2766)
 )
)

;; CHECK:      (type $0 (func))

;; CHECK:      (type $1 (func (result f64)))

;; CHECK:      (type $2 (func (param i32 f64 f64) (result i32)))

;; CHECK:      (type $3 (func (param i64)))

;; CHECK:      (type $4 (func (param f64) (result i32)))

;; CHECK:      (export "if-select" (func $0))

;; CHECK:      (export "unreachable-body-update-zext" (func $1))

;; CHECK:      (export "ssa-const" (func $2))

;; CHECK:      (export "if-nothing" (func $3))

;; CHECK:      (export "only-dfo" (func $4))

;; CHECK:      (func $0 (; has Stack IR ;)
;; CHECK-NEXT:  (nop)
;; CHECK-NEXT: )

;; CHECK:      (func $1 (; has Stack IR ;) (result f64)
;; CHECK-NEXT:  (unreachable)
;; CHECK-NEXT: )

;; CHECK:      (func $2 (; has Stack IR ;) (param $0 i32) (param $1 f64) (param $2 f64) (result i32)
;; CHECK-NEXT:  (unreachable)
;; CHECK-NEXT: )

;; CHECK:      (func $3 (; has Stack IR ;) (param $0 i64)
;; CHECK-NEXT:  (unreachable)
;; CHECK-NEXT: )

;; CHECK:      (func $4 (; has Stack IR ;) (param $0 f64) (result i32)
;; CHECK-NEXT:  (local $1 i32)
;; CHECK-NEXT:  (loop $label$1
;; CHECK-NEXT:   (if
;; CHECK-NEXT:    (i32.eqz
;; CHECK-NEXT:     (local.get $1)
;; CHECK-NEXT:    )
;; CHECK-NEXT:    (block
;; CHECK-NEXT:     (local.set $1
;; CHECK-NEXT:      (i32.const -2147483648)
;; CHECK-NEXT:     )
;; CHECK-NEXT:     (br $label$1)
;; CHECK-NEXT:    )
;; CHECK-NEXT:   )
;; CHECK-NEXT:  )
;; CHECK-NEXT:  (i32.const -2766)
;; CHECK-NEXT: )
