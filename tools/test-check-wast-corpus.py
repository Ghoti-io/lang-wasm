#!/usr/bin/env python3
"""Focused checks for the WAST feature-skip marker gate."""
import importlib.util
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("check-wast-corpus.py")
SPEC = importlib.util.spec_from_file_location("check_wast_corpus", SCRIPT)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class TypedFunctionReferenceMarkers(unittest.TestCase):
    def test_typed_reference_forms_are_markers(self):
        self.assertTrue(checker.contains_feature_marker("(ref null $T)", "typed-func-refs"))
        self.assertTrue(checker.contains_feature_marker("(param (ref 2))", "typed-func-refs"))
        self.assertTrue(checker.contains_feature_marker("call_ref", "typed-func-refs"))

    def test_abstract_function_and_extern_references_are_not_markers(self):
        self.assertFalse(checker.contains_feature_marker(
            "(ref null func) (ref null extern)", "typed-func-refs"))

    def test_identifiers_and_ordinary_bindings_are_not_markers(self):
        self.assertFalse(checker.contains_feature_marker(
            "(func $call_ref (param $x i32) (result i32) (local $y i32))",
            "typed-func-refs"))

    def test_strings_and_comments_are_not_markers(self):
        self.assertFalse(checker.contains_feature_marker(
            '(invoke "call_ref") ;; return_call_ref\n', "typed-func-refs"))
        self.assertFalse(checker.contains_feature_marker(
            '(module (custom "ref_as_non_null"))', "typed-func-refs"))

    def test_binary_markers_are_limited_to_module_binary_fragments(self):
        initialized_table = (
            '(module binary "\\00asm" "\\01\\00\\00\\00" '
            '"\\04\\0a\\01\\40\\00\\64\\70\\00\\01\\d2\\00\\0b")'
        )
        self.assertTrue(checker.contains_feature_marker(
            '(assert_invalid ' + initialized_table + ' "bad")',
            "typed-func-refs"))
        legacy_nonnull_table = (
            '(module binary "\\00asm" "\\01\\00\\00\\00" '
            '"\\04\\05\\01\\64\\70\\00\\01")'
        )
        self.assertTrue(checker.contains_feature_marker(
            legacy_nonnull_table, "typed-func-refs"))
        indexed_table = (
            '(module binary "\\00asm" "\\01\\00\\00\\00" '
            '"\\04\\05\\01\\63\\00\\00\\01")'
        )
        self.assertTrue(checker.contains_feature_marker(
            indexed_table, "typed-func-refs"))
        imported_table = (
            '(module binary "\\00asm" "\\01\\00\\00\\00" '
            '"\\02\\0a\\01\\01m\\01t\\01\\64\\00\\00\\01")'
        )
        self.assertTrue(checker.contains_feature_marker(
            imported_table, "typed-func-refs"))
        self.assertFalse(checker.contains_feature_marker(
            '(module binary "\\00asm" "\\01\\00\\00\\00" '
            '"\\00\\03\\01\\64\\70")', "typed-func-refs"))
        self.assertFalse(checker.contains_feature_marker(
            '(module binary "\\00asm" "\\01\\00\\00\\00" '
            '"\\04\\05\\01\\64\\6e\\00\\01")', "typed-func-refs"))
        typed_signature = (
            '(module binary "\\00asm" "\\01\\00\\00\\00" '
            '"\\01\\06\\01\\60\\01\\63\\00\\00")'
        )
        self.assertTrue(checker.contains_feature_marker(
            typed_signature, "typed-func-refs"))
        core_signature = (
            '(module binary "\\00asm" "\\01\\00\\00\\00" '
            '"\\01\\04\\01\\60\\01\\70\\00")'
        )
        self.assertFalse(checker.contains_feature_marker(
            core_signature, "typed-func-refs"))
        # A literal close parenthesis in an earlier custom-section payload is
        # a byte in the WAST string, not the end of the binary module form.
        with_literal_parenthesis = (
            '(module binary "\\00asm" "\\01\\00\\00\\00" '
            '"\\00\\03\\01x)" "\\04\\05\\01\\64\\70\\00\\01")'
        )
        self.assertTrue(checker.contains_feature_marker(
            with_literal_parenthesis, "typed-func-refs"))

    def test_invocation_skip_must_reach_a_matching_deferred_function(self):
        source = '(module\n  (func (export "call_ref-f") (call_ref $t))\n)\n'
        assertion = '(assert_trap (invoke "call_ref-f") "unreachable")'
        self.assertTrue(checker.assertion_contains_feature_marker(
            source + assertion, 4, assertion, "typed-func-refs"))
        unrelated = '(assert_trap (invoke "call_ref-f") "unreachable")'
        self.assertFalse(checker.assertion_contains_feature_marker(
            unrelated, 1, unrelated, "typed-func-refs"))
        earlier_module = (
            '(module\n  (func (export "call_ref-f") (call_ref $t))\n)\n'
            '(module\n  (func (export "call_ref-f"))\n)\n'
            '(assert_trap (invoke "call_ref-f") "unreachable")'
        )
        self.assertFalse(checker.assertion_contains_feature_marker(
            earlier_module, 8, '(assert_trap (invoke "call_ref-f") "unreachable")',
            "typed-func-refs"))
        one_line = (
            '(module (func (call_ref $t)) (export "call_ref-f" (func 0)))\n'
            '(assert_trap (invoke "call_ref-f") "unreachable")'
        )
        assertion = '(assert_trap (invoke "call_ref-f") "unreachable")'
        self.assertTrue(checker.assertion_contains_feature_marker(
            one_line, 2, assertion, "typed-func-refs"))
        same_line = (
            '(module (func (call_ref $t)) (export "call_ref-f" (func 0))) '
            '(assert_trap (invoke "call_ref-f") "unreachable")'
        )
        same_line_assertion = checker.source_form_at_line(same_line, 1)
        self.assertEqual(same_line_assertion, assertion)
        self.assertTrue(checker.assertion_contains_feature_marker(
            same_line, 1, same_line_assertion, "typed-func-refs"))
        imported = (
            '(module (import "m" "f" (func $f (param (ref $T)))) '
            '(export "call_ref-f" (func $f)))\n'
            '(assert_trap (invoke "call_ref-f") "unreachable")'
        )
        self.assertTrue(checker.assertion_contains_feature_marker(
            imported, 2, assertion, "typed-func-refs"))
        named_type = (
            '(module (type $sig (func (param (ref $T)))) '
            '(func (type $sig) (export "call_ref-f")))\n'
            '(assert_trap (invoke "call_ref-f") "unreachable")'
        )
        self.assertTrue(checker.assertion_contains_feature_marker(
            named_type, 2, assertion, "typed-func-refs"))
        registered = (
            '(module $old (func (call_ref $t)) (export "call_ref-f" (func 0)))\n'
            '(register "older" $old)\n'
            '(module (func (export "call_ref-f")))\n'
            '(assert_trap (invoke $older "call_ref-f") "unreachable")'
        )
        qualified = '(assert_trap (invoke $older "call_ref-f") "unreachable")'
        self.assertTrue(checker.assertion_contains_feature_marker(
            registered, 4, qualified, "typed-func-refs"))
        nested_only = (
            '(assert_invalid (module (func (export "call_ref-f") (call_ref $t))) "bad")\n'
            '(assert_trap (invoke "call_ref-f") "unreachable")'
        )
        self.assertFalse(checker.assertion_contains_feature_marker(
            nested_only, 2, assertion, "typed-func-refs"))

    def test_s33_decoder_rejects_out_of_range_fifth_byte(self):
        value, _ = checker._read_s33_leb(
            bytearray([0x80, 0x80, 0x80, 0x80, 0x10]), 0, 5)
        self.assertIsNone(value)


if __name__ == "__main__":
    unittest.main()
