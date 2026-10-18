;; Every iteration stores then reloads a deterministic word.
(module
  (memory 8 8)
  (func (export "run") (param $n i32) (result i64)
    (local $i i32) (local $sum i64) (local $value i32)
    (block $done
      (loop $again
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        (i32.store (i32.mul (local.get $i) (i32.const 4)) (local.get $i))
        (local.set $value
          (i32.load (i32.mul (local.get $i) (i32.const 4))))
        (local.set $sum
          (i64.add (local.get $sum) (i64.extend_i32_u (local.get $value))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $again)))
    (local.get $sum)))
