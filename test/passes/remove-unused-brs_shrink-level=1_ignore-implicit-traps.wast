(module
  (memory 256 256)
  (func $b14 (result i32)
    (drop
      (if (result i32) ;; with shrinking, this can become a select
        (i32.const 1)
        (block $block1 (result i32)
          (i32.const 12)
        )
        (block $block3 (result i32)
          (i32.const 27)
        )
      )
    )
    (drop
      (if (result i32)
        (i32.const 1)
        (i32.load (i32.const 10)) ;; load may have side effects, unless ignored
        (i32.const 27)
      )
    )
    (drop
      (if (result i32)
        (i32.const 1)
        (i32.rem_s (i32.const 11) (call $b14)) ;; rem may have side effects, unless ignored
        (i32.const 27)
      )
    )
    (drop
      (if (result i32)
        (i32.const 1)
        (i32.trunc_f64_u (f64.const 12.34)) ;; float to int may have side effects, unless ignored
        (i32.const 27)
      )
    )
    (drop
      (if (result i32)
        (i32.const 1)
        (i32.rem_s (i32.const 11) (i32.const 12)) ;; x / 12 has no side effects
        (i32.const 27)
      )
    )
    ;; TODO: tests without conditionalization that show this works.
    (i32.const 0)
  )
  (func $join-br_ifs
    (block $out
      (br_if $out (i32.const 1))
      (br_if $out (i32.const 2))
      (br_if $out (i32.const 3))
    )
    (block $out2
      (block $out3
        (br_if $out2 (i32.const 1))
        (br_if $out3 (i32.const 2))
        (br_if $out2 (i32.const 3))
      )
      (unreachable)
    )
    (block $out4
      (block $out5
        (br_if $out4 (i32.const 1))
        (br_if $out5 (i32.const 2))
        (br_if $out5 (i32.const 3))
      )
      (unreachable)
    )
    (block $out6
      (block $out7
        (br_if $out6 (i32.const 1))
        (br_if $out6 (i32.const 2))
        (br_if $out7 (i32.const 3))
      )
      (unreachable)
    )
    (block $out8
      (br_if $out8 (call $b14)) ;; side effect
      (br_if $out8 (i32.const 0))
    )
    (block $out8
      (br_if $out8 (i32.const 1))
      (br_if $out8 (call $b14)) ;; side effect
    )
  )
)

