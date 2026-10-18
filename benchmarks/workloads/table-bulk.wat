;; Every iteration observes both table.copy and table.fill through indirect calls.
(module
  (type $answer (func (result i32)))
  (table 4 4 funcref)
  (func $forty_two (type $answer) (result i32) (i32.const 42))
  (elem (i32.const 0) func $forty_two)
  (func (export "run") (param $n i32) (result i64)
    (local $i i32) (local $sum i64)
    (block $done
      (loop $again
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        (table.copy 0 0 (i32.const 1) (i32.const 0) (i32.const 1))
        (table.fill 0 (i32.const 2) (ref.func $forty_two) (i32.const 1))
        (local.set $sum
          (i64.add (local.get $sum)
            (i64.extend_i32_u
              (call_indirect (type $answer) (table 0) (i32.const 1)))))
        (local.set $sum
          (i64.add (local.get $sum)
            (i64.extend_i32_u
              (call_indirect (type $answer) (table 0) (i32.const 2)))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $again)))
    (local.get $sum)))
