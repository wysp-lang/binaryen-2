;; NOTE: Assertions have been generated by update_lit_checks.py and should not be edited.
;; (--remove-unused-names avoids names on blocks, which would hamper the
;; work in getFallthrough, as a name implies possible breaks)
;; RUN: wasm-opt %s -all --remove-unused-names --dse -S -o - | filecheck %s

(module
 (type $A (struct (field (mut i32))))
 (type $B (struct (field (mut f64))))

 (memory 10)

 (global $global$0 (mut i32) (i32.const 0))
 (global $global$1 (mut i32) (i32.const 0))

 ;; CHECK:      (func $simple-param (param $x (ref $A))
 ;; CHECK-NEXT:  (block
 ;; CHECK-NEXT:   (drop
 ;; CHECK-NEXT:    (local.get $x)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:   (drop
 ;; CHECK-NEXT:    (i32.const 10)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (block
 ;; CHECK-NEXT:   (drop
 ;; CHECK-NEXT:    (local.get $x)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:   (drop
 ;; CHECK-NEXT:    (i32.const 20)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $simple-param (param $x (ref $A))
  (struct.set $A 0
   (local.get $x)
   (i32.const 10)
  )
  (struct.set $A 0
   (local.get $x)
   (i32.const 20)
  )
  ;; the last store escapes to the outside, and cannot be modified
  (struct.set $A 0
   (local.get $x)
   (i32.const 30)
  )
 )

 ;; TODO: test with locals when non-nullable locals are possible

 ;; CHECK:      (func $simple-fallthrough (param $x (ref $A))
 ;; CHECK-NEXT:  (block
 ;; CHECK-NEXT:   (drop
 ;; CHECK-NEXT:    (block (result (ref $A))
 ;; CHECK-NEXT:     (local.get $x)
 ;; CHECK-NEXT:    )
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:   (drop
 ;; CHECK-NEXT:    (i32.const 10)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (block (result (ref $A))
 ;; CHECK-NEXT:    (local.get $x)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:   (i32.const 20)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $simple-fallthrough (param $x (ref $A))
  (struct.set $A 0
   (block (result (ref $A))
    (local.get $x)
   )
   (i32.const 10)
  )
  (struct.set $A 0
   (block (result (ref $A))
    (local.get $x)
   )
   (i32.const 20)
  )
 )

 ;; CHECK:      (func $get-ref (result (ref $A))
 ;; CHECK-NEXT:  (unreachable)
 ;; CHECK-NEXT: )
 (func $get-ref (result (ref $A))
  (unreachable)
 )

 ;; CHECK:      (func $ref-changes (param $x (ref $A))
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (local.set $x
 ;; CHECK-NEXT:   (call $get-ref)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 20)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $ref-changes (param $x (ref $A))
  (struct.set $A 0
   (local.get $x)
   (i32.const 10)
  )
  (local.set $x
   (call $get-ref)
  )
  (struct.set $A 0
   (local.get $x)
   (i32.const 20)
  )
 )

 ;; CHECK:      (func $ref-may-change (param $x (ref $A)) (param $i i32)
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (if
 ;; CHECK-NEXT:   (local.get $i)
 ;; CHECK-NEXT:   (local.set $x
 ;; CHECK-NEXT:    (call $get-ref)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 20)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $ref-may-change (param $x (ref $A)) (param $i i32)
  (struct.set $A 0
   (local.get $x)
   (i32.const 10)
  )
  (if
   (local.get $i)
   (local.set $x
    (call $get-ref)
   )
  )
  (struct.set $A 0
   (local.get $x)
   (i32.const 20)
  )
 )

 ;; CHECK:      (func $simple-use (param $x (ref $A))
 ;; CHECK-NEXT:  (block
 ;; CHECK-NEXT:   (drop
 ;; CHECK-NEXT:    (local.get $x)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:   (drop
 ;; CHECK-NEXT:    (i32.const 10)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 20)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (drop
 ;; CHECK-NEXT:   (struct.get $A 0
 ;; CHECK-NEXT:    (local.get $x)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $simple-use (param $x (ref $A))
  (struct.set $A 0
   (local.get $x)
   (i32.const 10)
  )
  (struct.set $A 0
   (local.get $x)
   (i32.const 20)
  )
  (drop
   (struct.get $A 0
    (local.get $x)
   )
  )
  (struct.set $A 0
   (local.get $x)
   (i32.const 30)
  )
 )

 ;; CHECK:      (func $two-types (param $x (ref $A)) (param $y (ref $B))
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (struct.set $B 0
 ;; CHECK-NEXT:   (local.get $y)
 ;; CHECK-NEXT:   (f64.const 20)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $two-types (param $x (ref $A)) (param $y (ref $B))
  (struct.set $A 0
   (local.get $x)
   (i32.const 10)
  )
  ;; the simple analysis currently gives up on a set we cannot easily classify
  (struct.set $B 0
   (local.get $y)
   (f64.const 20)
  )
  (struct.set $A 0
   (local.get $x)
   (i32.const 30)
  )
 )

 ;; CHECK:      (func $two-types-get (param $x (ref $A)) (param $y (ref $B))
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (drop
 ;; CHECK-NEXT:   (struct.get $B 0
 ;; CHECK-NEXT:    (local.get $y)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $two-types-get (param $x (ref $A)) (param $y (ref $B))
  (struct.set $A 0
   (local.get $x)
   (i32.const 10)
  )
  ;; the simple analysis currently gives up on a set we cannot easily classify
  (drop
   (struct.get $B 0
    (local.get $y)
   )
  )
  (struct.set $A 0
   (local.get $x)
   (i32.const 30)
  )
 )

 (func $foo)

 ;; CHECK:      (func $call (param $x (ref $A))
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (call $foo)
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $call (param $x (ref $A))
  (struct.set $A 0
   (local.get $x)
   (i32.const 10)
  )
  ;; the analysis gives up on a call
  (call $foo)
  (struct.set $A 0
   (local.get $x)
   (i32.const 30)
  )
 )

 ;; CHECK:      (func $through-branches (param $x (ref $A))
 ;; CHECK-NEXT:  (block
 ;; CHECK-NEXT:   (drop
 ;; CHECK-NEXT:    (local.get $x)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:   (drop
 ;; CHECK-NEXT:    (i32.const 10)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (if
 ;; CHECK-NEXT:   (i32.const 1)
 ;; CHECK-NEXT:   (nop)
 ;; CHECK-NEXT:   (nop)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $through-branches (param $x (ref $A))
  (struct.set $A 0
   (local.get $x)
   (i32.const 10)
  )
  ;; the analysis is not confused by branching
  (if (i32.const 1)
   (nop)
   (nop)
  )
  (struct.set $A 0
   (local.get $x)
   (i32.const 30)
  )
 )

 ;; CHECK:      (func $just-one-branch-trample (param $x (ref $A))
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (if
 ;; CHECK-NEXT:   (i32.const 1)
 ;; CHECK-NEXT:   (struct.set $A 0
 ;; CHECK-NEXT:    (local.get $x)
 ;; CHECK-NEXT:    (i32.const 20)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:   (nop)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $just-one-branch-trample (param $x (ref $A))
  (struct.set $A 0
   (local.get $x)
   (i32.const 10)
  )
  ;; a trample on just one branch is not enough
  (if (i32.const 1)
   (struct.set $A 0
    (local.get $x)
    (i32.const 20)
   )
   (nop)
  )
 )

 ;; CHECK:      (func $just-one-branch-bad (param $x (ref $A))
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (if
 ;; CHECK-NEXT:   (i32.const 1)
 ;; CHECK-NEXT:   (call $foo)
 ;; CHECK-NEXT:   (nop)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $just-one-branch-bad (param $x (ref $A))
  (struct.set $A 0
   (local.get $x)
   (i32.const 10)
  )
  ;; an unknown interaction on one branch is enough to make us give up
  (if (i32.const 1)
   (call $foo)
   (nop)
  )
  (struct.set $A 0
   (local.get $x)
   (i32.const 30)
  )
 )

 ;; CHECK:      (func $simple-in-branches (param $x (ref $A))
 ;; CHECK-NEXT:  (if
 ;; CHECK-NEXT:   (i32.const 1)
 ;; CHECK-NEXT:   (block
 ;; CHECK-NEXT:    (block
 ;; CHECK-NEXT:     (drop
 ;; CHECK-NEXT:      (local.get $x)
 ;; CHECK-NEXT:     )
 ;; CHECK-NEXT:     (drop
 ;; CHECK-NEXT:      (i32.const 10)
 ;; CHECK-NEXT:     )
 ;; CHECK-NEXT:    )
 ;; CHECK-NEXT:    (struct.set $A 0
 ;; CHECK-NEXT:     (local.get $x)
 ;; CHECK-NEXT:     (i32.const 20)
 ;; CHECK-NEXT:    )
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:   (block
 ;; CHECK-NEXT:    (block
 ;; CHECK-NEXT:     (drop
 ;; CHECK-NEXT:      (local.get $x)
 ;; CHECK-NEXT:     )
 ;; CHECK-NEXT:     (drop
 ;; CHECK-NEXT:      (i32.const 30)
 ;; CHECK-NEXT:     )
 ;; CHECK-NEXT:    )
 ;; CHECK-NEXT:    (struct.set $A 0
 ;; CHECK-NEXT:     (local.get $x)
 ;; CHECK-NEXT:     (i32.const 40)
 ;; CHECK-NEXT:    )
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $simple-in-branches (param $x (ref $A))
  (if (i32.const 1)
   (block
    (struct.set $A 0
     (local.get $x)
     (i32.const 10)
    )
    (struct.set $A 0
     (local.get $x)
     (i32.const 20)
    )
   )
   (block
    (struct.set $A 0
     (local.get $x)
     (i32.const 30)
    )
    (struct.set $A 0
     (local.get $x)
     (i32.const 40)
    )
   )
  )
 )

 ;; CHECK:      (func $different-refs-same-type (param $x (ref $A)) (param $y (ref $A))
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $y)
 ;; CHECK-NEXT:   (i32.const 20)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (struct.set $A 0
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $different-refs-same-type (param $x (ref $A)) (param $y (ref $A))
  (struct.set $A 0
   (local.get $x)
   (i32.const 10)
  )
  (struct.set $A 0
   (local.get $y)
   (i32.const 20)
  )
  ;; the last store escapes to the outside, and cannot be modified
  (struct.set $A 0
   (local.get $x)
   (i32.const 30)
  )
 )

 ;; CHECK:      (func $no-basic-blocks
 ;; CHECK-NEXT:  (unreachable)
 ;; CHECK-NEXT: )
 (func $no-basic-blocks
  (unreachable)
 )

 ;; CHECK:      (func $global
 ;; CHECK-NEXT:  (drop
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (global.set $global$1
 ;; CHECK-NEXT:   (i32.const 20)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (global.set $global$0
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $global
  ;; globals are optimized as well, and we have more precise data there than on
  ;; GC references - aliasing is impossible.
  (global.set $global$0
   (i32.const 10)
  )
  (global.set $global$1
   (i32.const 20)
  )
  (global.set $global$0
   (i32.const 30)
  )
 )

 ;; CHECK:      (func $global-trap
 ;; CHECK-NEXT:  (global.set $global$0
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (if
 ;; CHECK-NEXT:   (i32.const 1)
 ;; CHECK-NEXT:   (unreachable)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (global.set $global$0
 ;; CHECK-NEXT:   (i32.const 20)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $global-trap
  (global.set $global$0
   (i32.const 10)
  )
  ;; a trap (even conditional) prevents our optimizations, global state may be
  ;; observed if another export is called later after the trap.
  (if
   (i32.const 1)
   (unreachable)
  )
  (global.set $global$0
   (i32.const 20)
  )
 )

 ;; CHECK:      (func $memory-const
 ;; CHECK-NEXT:  (block
 ;; CHECK-NEXT:   (drop
 ;; CHECK-NEXT:    (i32.const 10)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:   (drop
 ;; CHECK-NEXT:    (i32.const 20)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (i32.store
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $memory-const
  (i32.store
   (i32.const 10)
   (i32.const 20)
  )
  (i32.store
   (i32.const 10)
   (i32.const 30)
  )
 )

 ;; CHECK:      (func $memory-param (param $x i32)
 ;; CHECK-NEXT:  (block
 ;; CHECK-NEXT:   (drop
 ;; CHECK-NEXT:    (local.get $x)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:   (drop
 ;; CHECK-NEXT:    (i32.const 20)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (i32.store
 ;; CHECK-NEXT:   (local.get $x)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $memory-param (param $x i32)
  (i32.store
   (local.get $x)
   (i32.const 20)
  )
  (i32.store
   (local.get $x)
   (i32.const 30)
  )
 )

 ;; CHECK:      (func $memory-wrong-const
 ;; CHECK-NEXT:  (i32.store
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:   (i32.const 20)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (i32.store
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:   (i32.const 40)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $memory-wrong-const
  (i32.store
   (i32.const 10)
   (i32.const 20)
  )
  (i32.store
   (i32.const 30)
   (i32.const 40)
  )
 )

 ;; CHECK:      (func $memory-wrong-offset
 ;; CHECK-NEXT:  (i32.store
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:   (i32.const 20)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (i32.store offset=1
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $memory-wrong-offset
  (i32.store
   (i32.const 10)
   (i32.const 20)
  )
  (i32.store offset=1
   (i32.const 10)
   (i32.const 30)
  )
 )

 ;; CHECK:      (func $memory-wrong-size
 ;; CHECK-NEXT:  (i32.store
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:   (i32.const 20)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (i32.store16
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $memory-wrong-size
  (i32.store
   (i32.const 10)
   (i32.const 20)
  )
  (i32.store16
   (i32.const 10)
   (i32.const 30)
  )
 )

 ;; CHECK:      (func $memory-other-interference
 ;; CHECK-NEXT:  (i32.store
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:   (i32.const 20)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (memory.fill
 ;; CHECK-NEXT:   (i32.const 0)
 ;; CHECK-NEXT:   (i32.const 0)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (i32.store
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $memory-other-interference
  (i32.store
   (i32.const 10)
   (i32.const 20)
  )
  (memory.fill
   (i32.const 0)
   (i32.const 0)
   (i32.const 30)
  )
  (i32.store
   (i32.const 10)
   (i32.const 30)
  )
 )

 ;; CHECK:      (func $memory-load
 ;; CHECK-NEXT:  (i32.store
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:   (i32.const 20)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (drop
 ;; CHECK-NEXT:   (i32.load
 ;; CHECK-NEXT:    (i32.const 10)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (i32.store
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $memory-load
  (i32.store
   (i32.const 10)
   (i32.const 20)
  )
  (drop
   (i32.load
    (i32.const 10)
   )
  )
  (i32.store
   (i32.const 10)
   (i32.const 30)
  )
 )

 ;; CHECK:      (func $memory-load-wrong-offset
 ;; CHECK-NEXT:  (i32.store
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:   (i32.const 20)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (drop
 ;; CHECK-NEXT:   (i32.load offset=1
 ;; CHECK-NEXT:    (i32.const 10)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (i32.store
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $memory-load-wrong-offset
  (i32.store
   (i32.const 10)
   (i32.const 20)
  )
  (drop
   (i32.load offset=1
    (i32.const 10)
   )
  )
  (i32.store
   (i32.const 10)
   (i32.const 30)
  )
 )

 ;; CHECK:      (func $memory-load-wrong-bytes
 ;; CHECK-NEXT:  (i32.store
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:   (i32.const 20)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (drop
 ;; CHECK-NEXT:   (i32.load8_s
 ;; CHECK-NEXT:    (i32.const 10)
 ;; CHECK-NEXT:   )
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT:  (i32.store
 ;; CHECK-NEXT:   (i32.const 10)
 ;; CHECK-NEXT:   (i32.const 30)
 ;; CHECK-NEXT:  )
 ;; CHECK-NEXT: )
 (func $memory-load-wrong-bytes
  (i32.store
   (i32.const 10)
   (i32.const 20)
  )
  (drop
   (i32.load8_s
    (i32.const 10)
   )
  )
  (i32.store
   (i32.const 10)
   (i32.const 30)
  )
 )

 ;; TODO: test try throwing
)
