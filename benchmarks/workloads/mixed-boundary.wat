;; A host import is dispatched through a runtime-core boundary fixture.
(module
  (import "bench.v1" "boundary" (func $boundary (param i32) (result i32)))
  (func (export "nested") (param $value i32) (result i32)
    (i32.add (local.get $value) (i32.const 1)))
  (func (export "run") (param $n i32) (result i64)
    (local $i i32) (local $sum i64) (local $value i32)
    (block $done
      (loop $again
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        (local.set $value (call $boundary (local.get $i)))
        (local.set $sum
          (i64.add (local.get $sum) (i64.extend_i32_u (local.get $value))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $again)))
    (local.get $sum)))
