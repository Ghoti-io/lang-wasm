;; The runner passes one i32 guest-work count and verifies sum(0..n-1).
(module
  (func (export "run") (param $n i32) (result i64)
    (local $i i32) (local $sum i64)
    (block $done
      (loop $again
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        (local.set $sum
          (i64.add (local.get $sum) (i64.extend_i32_u (local.get $i))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $again)))
    (local.get $sum)))
