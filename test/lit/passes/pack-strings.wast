;; RUN: wasm-opt %s --pack-strings=str.const,%s.json,%t -all -S -o - | filecheck %s

(module
  ;; String constants. Their indexes will be packed, from 0,1,3,5 to 0,1,2,3.
  ;;
  ;; Note that 5 appears before 3, intentionally, to see that we are not
  ;; affected by the order of imports. Only the indexes matter to us.

  ;; CHECK:      (import "str.const" "0" (global $hello (ref extern)))
  ;; CHECK:      (import "str.const" "1" (global $world (ref extern)))
  ;; CHECK:      (import "str.const" "2" (global $bye (ref extern)))
  ;; CHECK:      (import "str.const" "3" (global $later (ref extern)))
  (import "str.const" "0" (global $hello (ref extern)))
  (import "str.const" "1" (global $world (ref extern)))
  (import "str.const" "5" (global $bye (ref extern)))
  (import "str.const" "3" (global $later (ref extern)))

  ;; A different module name. This is not modified, and it does not affect the
  ;; others.

  ;; CHECK:      (import "string.const" "2" (global $other (ref extern)))
  (import "string.const" "2" (global $other (ref extern)))
)

;; RUN: cat %t | filecheck %s --check-prefix=OUTPUT

;; Note how "bye" and "later" have flipped in order compared to the original
;; JSON (because that is their order in the new packed indexing, which is
;; determined by the order of imports).
;; OUTPUT: ["hello", "world", "bye", "later"]
