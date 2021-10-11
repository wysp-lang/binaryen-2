;; NOTE: Assertions have been generated by update_lit_checks.py --all-items and should not be edited.
;; RUN: foreach %s %t wasm-opt --nominal --gto -all -S -o - | filecheck %s
;; (remove-unused-names is added to test fallthrough values without a block
;; name getting in the way)

(module
  ;; The struct here has three fields, and the second of them has no struct.set
  ;; which means we can make it immutable.

  ;; CHECK:      (type $struct (struct (field (mut funcref)) (field funcref)))
  (type $struct (struct (field (mut funcref)) (field (mut funcref)) (field (mut funcref))))

  ;; Test that we update tag types properly.
  ;; CHECK:      (type $ref|$struct|_=>_none (func (param (ref $struct))))

  ;; CHECK:      (type $none_=>_ref?|$struct| (func (result (ref null $struct))))

  ;; CHECK:      (tag $tag (param (ref $struct)))
  (tag $tag (param (ref $struct)))

  ;; CHECK:      (func $func (param $x (ref $struct))
  ;; CHECK-NEXT:  (local $temp (ref null $struct))
  ;; CHECK-NEXT:  (drop
  ;; CHECK-NEXT:   (struct.new $struct
  ;; CHECK-NEXT:    (ref.null func)
  ;; CHECK-NEXT:    (ref.null func)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (struct.set $struct 0
  ;; CHECK-NEXT:   (local.get $x)
  ;; CHECK-NEXT:   (ref.null func)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (block
  ;; CHECK-NEXT:   (drop
  ;; CHECK-NEXT:    (local.get $x)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:   (drop
  ;; CHECK-NEXT:    (ref.null func)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (local.set $temp
  ;; CHECK-NEXT:   (local.get $x)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (drop
  ;; CHECK-NEXT:   (struct.get $struct 0
  ;; CHECK-NEXT:    (local.get $x)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (drop
  ;; CHECK-NEXT:   (struct.get $struct 1
  ;; CHECK-NEXT:    (local.get $x)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $func (param $x (ref $struct))
    (local $temp (ref null $struct))
    ;; The presence of a struct.new does not prevent this optimization: we just
    ;; care about writes using struct.set.
    (drop
      (struct.new $struct
        (ref.null func)
        (ref.null func)
        (ref.null func)
      )
    )
    (struct.set $struct 0
      (local.get $x)
      (ref.null func)
    )
    (struct.set $struct 2
      (local.get $x)
      (ref.null func)
    )
    ;; Test that local types remain valid after our work (otherwise, we'd get a
    ;; validation error).
    (local.set $temp
      (local.get $x)
    )
    ;; Test that struct.get types remain valid after our work.
    (drop
      (struct.get $struct 0
        (local.get $x)
      )
    )
    (drop
      (struct.get $struct 1
        (local.get $x)
      )
    )
  )

  ;; CHECK:      (func $foo (result (ref null $struct))
  ;; CHECK-NEXT:  (try $try
  ;; CHECK-NEXT:   (do
  ;; CHECK-NEXT:    (nop)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:   (catch $tag
  ;; CHECK-NEXT:    (return
  ;; CHECK-NEXT:     (pop (ref $struct))
  ;; CHECK-NEXT:    )
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (ref.null $struct)
  ;; CHECK-NEXT: )
  (func $foo (result (ref null $struct))
    ;; Use a tag so that we test proper updating of its type after making
    ;; changes.
    (try
      (do
        (nop)
      )
      (catch $tag
        (return
          (pop (ref $struct))
        )
      )
    )
    (ref.null $struct)
  )
)

(module
  ;; Test recursion between structs where we only modify one. Specifically $B
  ;; has no writes to either of its fields.

  ;; CHECK:      (type $ref|$A|_=>_none (func (param (ref $A))))

  ;; CHECK:      (type $B (struct ))

  ;; CHECK:      (type $A (struct ))
  (type $A (struct (field (mut (ref null $B))) (field (mut i32)) ))
  (type $B (struct (field (mut (ref null $A))) (field (mut f64)) ))

  ;; CHECK:      (func $func (param $x (ref $A))
  ;; CHECK-NEXT:  (block
  ;; CHECK-NEXT:   (drop
  ;; CHECK-NEXT:    (local.get $x)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:   (drop
  ;; CHECK-NEXT:    (ref.null $B)
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
  ;; CHECK-NEXT: )
  (func $func (param $x (ref $A))
    (struct.set $A 0
      (local.get $x)
      (ref.null $B)
    )
    (struct.set $A 1
      (local.get $x)
      (i32.const 20)
    )
  )
)

(module
  ;; As before, but flipped so that $A's fields can become immutable.

  ;; CHECK:      (type $ref|$B|_=>_none (func (param (ref $B))))

  ;; CHECK:      (type $A (struct ))

  ;; CHECK:      (type $B (struct ))
  (type $B (struct (field (mut (ref null $A))) (field (mut f64)) ))

  (type $A (struct (field (mut (ref null $B))) (field (mut i32)) ))

  ;; CHECK:      (func $func (param $x (ref $B))
  ;; CHECK-NEXT:  (block
  ;; CHECK-NEXT:   (drop
  ;; CHECK-NEXT:    (local.get $x)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:   (drop
  ;; CHECK-NEXT:    (ref.null $A)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (block
  ;; CHECK-NEXT:   (drop
  ;; CHECK-NEXT:    (local.get $x)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:   (drop
  ;; CHECK-NEXT:    (f64.const 3.14159)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $func (param $x (ref $B))
    (struct.set $B 0
      (local.get $x)
      (ref.null $A)
    )
    (struct.set $B 1
      (local.get $x)
      (f64.const 3.14159)
    )
  )
)

(module
  ;; As before, but now one field in each can become immutable.

  ;; CHECK:      (type $B (struct ))
  (type $B (struct (field (mut (ref null $A))) (field (mut f64)) ))

  ;; CHECK:      (type $ref|$A|_ref|$B|_=>_none (func (param (ref $A) (ref $B))))

  ;; CHECK:      (type $A (struct ))
  (type $A (struct (field (mut (ref null $B))) (field (mut i32)) ))

  ;; CHECK:      (func $func (param $x (ref $A)) (param $y (ref $B))
  ;; CHECK-NEXT:  (block
  ;; CHECK-NEXT:   (drop
  ;; CHECK-NEXT:    (local.get $x)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:   (drop
  ;; CHECK-NEXT:    (ref.null $B)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (block
  ;; CHECK-NEXT:   (drop
  ;; CHECK-NEXT:    (local.get $y)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:   (drop
  ;; CHECK-NEXT:    (f64.const 3.14159)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $func (param $x (ref $A)) (param $y (ref $B))
    (struct.set $A 0
      (local.get $x)
      (ref.null $B)
    )
    (struct.set $B 1
      (local.get $y)
      (f64.const 3.14159)
    )
  )
)

(module
  ;; Field #0 is already immutable.
  ;; Field #1 is mutable and can become so.
  ;; Field #2 is mutable and must remain so.

  ;; CHECK:      (type $ref|$struct|_=>_none (func (param (ref $struct))))

  ;; CHECK:      (type $struct (struct ))
  (type $struct (struct (field i32) (field (mut i32)) (field (mut i32))))

  ;; CHECK:      (func $func (param $x (ref $struct))
  ;; CHECK-NEXT:  (drop
  ;; CHECK-NEXT:   (local.get $x)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (drop
  ;; CHECK-NEXT:   (i32.const 1)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $func (param $x (ref $struct))
    (struct.set $struct 2
      (local.get $x)
      (i32.const 1)
    )
  )
)

(module
  ;; Subtyping. Without a write in either supertype or subtype, we can
  ;; optimize the field to be immutable.

  ;; CHECK:      (type $none_=>_none (func))

  ;; CHECK:      (type $super (struct ))
  (type $super (struct (field (mut i32))))
  ;; CHECK:      (type $sub (struct ) (extends $super))
  (type $sub (struct (field (mut i32))) (extends $super))

  ;; CHECK:      (func $func
  ;; CHECK-NEXT:  (drop
  ;; CHECK-NEXT:   (struct.new_default $super)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (drop
  ;; CHECK-NEXT:   (struct.new_default $sub)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $func
    ;; The presence of struct.new do not prevent us optimizing
    (drop
      (struct.new $super
        (i32.const 1)
      )
    )
    (drop
      (struct.new $sub
        (i32.const 1)
      )
    )
  )
)

(module
  ;; As above, but add a write in the super, which prevents optimization.

  ;; CHECK:      (type $super (struct ))
  (type $super (struct (field (mut i32))))
  ;; CHECK:      (type $ref|$super|_=>_none (func (param (ref $super))))

  ;; CHECK:      (type $sub (struct ) (extends $super))
  (type $sub (struct (field (mut i32))) (extends $super))

  ;; CHECK:      (func $func (param $x (ref $super))
  ;; CHECK-NEXT:  (drop
  ;; CHECK-NEXT:   (struct.new_default $super)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (drop
  ;; CHECK-NEXT:   (struct.new_default $sub)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (block
  ;; CHECK-NEXT:   (drop
  ;; CHECK-NEXT:    (local.get $x)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:   (drop
  ;; CHECK-NEXT:    (i32.const 2)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $func (param $x (ref $super))
    ;; The presence of struct.new do not prevent us optimizing
    (drop
      (struct.new $super
        (i32.const 1)
      )
    )
    (drop
      (struct.new $sub
        (i32.const 1)
      )
    )
    (struct.set $super 0
      (local.get $x)
      (i32.const 2)
    )
  )
)

(module
  ;; As above, but add a write in the sub, which prevents optimization.

  ;; CHECK:      (type $ref|$sub|_=>_none (func (param (ref $sub))))

  ;; CHECK:      (type $sub (struct ) (extends $super))

  ;; CHECK:      (type $super (struct ))
  (type $super (struct (field (mut i32))))
  (type $sub (struct (field (mut i32))) (extends $super))

  ;; CHECK:      (func $func (param $x (ref $sub))
  ;; CHECK-NEXT:  (drop
  ;; CHECK-NEXT:   (local.get $x)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (drop
  ;; CHECK-NEXT:   (i32.const 2)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $func (param $x (ref $sub))
    (struct.set $sub 0
      (local.get $x)
      (i32.const 2)
    )
  )
)
