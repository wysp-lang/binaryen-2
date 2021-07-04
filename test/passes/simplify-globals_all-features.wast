(module
  (import "env" "global-1" (global $g1 i32))
  (global $g2 (mut i32) (global.get $g1))
  (func $foo
   (drop (global.get $g1))
   ;; $g2 is initialized to $g1, and never written to, so we can optimize the
   ;; read to look at $g1.
   (drop (global.get $g2))
  )
)
(module
  (import "env" "global-1" (global $g1 i32))
  (global $g2 (mut i32) (global.get $g1))
  (global $g3 (mut i32) (global.get $g2))
  (global $g4 (mut i32) (global.get $g3))
  (func $foo
   ;; As above, but with long chains of initializations.
   (drop (global.get $g1))
   (drop (global.get $g2))
   (drop (global.get $g3))
   (drop (global.get $g4))
  )
)
(module
  ;; Unused globals are not removed in this pass; we leave that for others.
  (import "env" "global-1" (global $g1 (mut i32)))
  (global $g2 (mut i32) (global.get $g1))
)
;; A set of a global that is never read can be removed.
(module
  (global $write-only (mut i32) (i32.const 1))
  (func $foo
    (global.set $write-only (i32.const 2))
  )
)
;; Similar, but with an unreachable value.
(module
  (global $write-only (mut i32) (i32.const 1))
  (func $foo
    (global.set $write-only (unreachable))
  )
)
(module
  (global $g1 i32 (i32.const 1))
  (global $g2 i32 (global.get $g1))
  (global $g3 f64 (f64.const -3.4))
  (global $g4 (mut f64) (f64.const -2.8))
  (global $g5 i32 (i32.const 2))
  (global $g6 (mut i32) (global.get $g5))
  (global $g7 (mut i32) (i32.const 3))
  (global $g8 i32 (global.get $g7))
  (global $g9 i32 (i32.const 4))
  (global $ga (mut i32) (global.get $g9))
  (global $gb (mut i32) (i32.const 5))
  (global $gc i32 (global.get $gb))
  (func $foo
   ;; All these values can be applied at compile time. While some of the
   ;; locals are mutable, they are never written to.
   (drop (global.get $g1))
   (drop (global.get $g2))
   (drop (global.get $g3))
   (drop (global.get $g4))
   (drop (global.get $g5))
   (drop (global.get $g6))
   (drop (global.get $g7))
   (drop (global.get $g8))
   (drop (global.get $g9))
   ;; However, these two are in fact written to (see below), and so they cannot
   ;; be optimized.
   (drop (global.get $ga))
   (drop (global.get $gb))
   ;; This one *can* be optimized, even though its initial value depends on
   ;; a mutable global. The mutable global cannot be modified before the
   ;; initial value reaches this local, so we know the initial value of $gc.
   (drop (global.get $gc))

   ;; Some sets to make $ga, $gb written to.
   (global.set $ga (i32.const 6))
   (global.set $gb (i32.const 7))
  )
)
(module
  (global $g1 (mut i32) (i32.const 1))
  (global $g2 (mut i32) (i32.const 1))
  (func $f (param $x i32) (result i32)
    ;; Some sets of globals, followed by control flow branching, so we do
    ;; not propagate any values here.
    (global.set $g1 (i32.const 100))
    (global.set $g2 (local.get $x))
    (if (local.get $x) (return (i32.const 0)))
    (local.set $x
      (i32.add
        (global.get $g1)
        (global.get $g2)
      )
    )
    (if (local.get $x) (return (i32.const 1)))
    ;; Some set sof globals that are immediately used, with no interference
    ;; in the middle, so we *can* forward the values. $g1 is forwarded as
    ;; we know it, whle $g2, which is not a constant, is not.
    (global.set $g1 (i32.const 200))
    (global.set $g2 (local.get $x))
    (local.set $x
      (i32.add
        (global.get $g1)
        (global.get $g2)
      )
    )
    (local.get $x)
  )
)
(module
  (global $g1 (mut i32) (i32.const 1))
  (global $g2 (mut i32) (i32.const 1))
  (func $f (param $x i32) (result i32)
    (global.set $g1 (i32.const 100))
    (global.set $g2 (local.get $x))
    (local.set $x
      (i32.add
        (i32.add
          ;; The constant value of $g1 should be applied in both appearances
          ;; here.
          (global.get $g1)
          (global.get $g1)
        )
        (global.get $g2)
      )
    )
    (local.get $x)
  )
)
;; Test for things invalidating our potentially-propagated values, because
;; something interferes along the way.
(module
  (global $g1 (mut i32) (i32.const 1))
  (global $g2 (mut i32) (i32.const 1))
  (func $no (param $x i32) (result i32)
    (global.set $g1 (i32.const 100))
    (drop (call $no (i32.const 200))) ;; invalidate
    (global.get $g1)
  )
  (func $no2 (param $x i32) (result i32)
    (global.set $g1 (i32.const 100))
    (global.set $g1 (local.get $x)) ;; invalidate
    (global.get $g1)
  )
  (func $yes (param $x i32) (result i32)
    (global.set $g1 (i32.const 100))
    (global.set $g2 (local.get $x)) ;; almost invalidate, but different global
    (global.get $g1)
  )
)
;; Reference type tests
(module
  (import "env" "global-1" (global $g1 externref))
  (global $g2 (mut externref) (global.get $g1))
  (global $g3 externref (ref.null extern))
  (func $test1
    (drop (global.get $g1))
    (drop (global.get $g2))
  )
  (func $test2
    (drop (global.get $g3))
  )
)
