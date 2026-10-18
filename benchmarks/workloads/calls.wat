;; Every iteration performs one direct call and one indirect call.
(module
  (type $add (func (param i32 i32) (result i32)))
  (table 1 funcref)
  (elem (i32.const 0) func $step)
  (func $step (type $add) (param $a i32) (param $b i32) (result i32)
    (i32.add (local.get $a) (local.get $b)))
  (func (export "run") (param $n i32) (result i64)
    (local $i i32) (local $sum i64) (local $v i32)
    (block $done
      (loop $again
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        (local.set $v (call $step (local.get $i) (i32.const 1)))
        (local.set $v
          (call_indirect (type $add) (local.get $v) (i32.const 1)
            (i32.const 0)))
        (local.set $sum
          (i64.add (local.get $sum) (i64.extend_i32_u (local.get $v))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $again)))
    (local.get $sum)))
