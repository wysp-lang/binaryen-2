(module
  (import "env" "func" (func $import))
  (func $nopp
    (nop)
  )
  (func $intt (result i32)
    (i32.const 10)
  )
  (func $workk
    (if (i32.const 0) (nop))
    (drop (i32.const 1))
  )
  (func $loops
    (loop $x
      (call $loops)
      (br $x)
    )
    (if (call $intt)
      (loop $y
        (call $loops)
        (br $y)
      )
    )
    (loop $z
      (drop (i32.const 10))
      (drop (i32.const 20))
      (drop (i32.const 30))
      (br $z)
    )
  )
  (func $loops-similar
    (loop $x
      (call $loops)
      (br $x)
    )
  )
  (func $blocks
    (block $x
      (br $x)
    )
    (block $y
      (unreachable)
    )
    (block $z
      (br_if $z (i32.const 1))
    )
    (drop
      (block $w (result i32)
        (br $w (i32.const 2))
      )
    )
    (drop
      (block $v (result i32)
        (drop (br_if $v (i32.const 3) (i32.const 4)))
        (i32.const 5)
      )
    )
    (drop
      (block $u (result i32)
        (drop (br_if $u (i32.const 5) (i32.const 6)))
        (unreachable)
      )
    )
  )
  (func $ifs
    (if (i32.const 1)
      (drop (i32.const 2))
    )
    (if (i32.const 3)
      (drop (i32.const 4))
      (drop (i32.const 5))
    )
    (if (i32.const 6)
      (block
        (drop (i32.const 7))
        (drop (i32.const 8))
        (drop (i32.const 9))
      )
      (block
        (drop (i32.const 10))
        (drop (i32.const 11))
        (drop (i32.const 12))
      )
    )
  )
  (func $returns
    (if (i32.const 1)
      (return)
    )
    (return)
  )
  (func $return-values (result i32)
    (if (i32.const 1)
      (return (i32.const 2))
    )
    (if (i32.const 3)
      (return (i32.const 4))
    )
    (return (i32.const 5))
  )
)

