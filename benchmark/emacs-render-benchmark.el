;;; emacs-render-benchmark.el --- Benchmark suite for Emacs rendering backends -*- lexical-binding: t -*-

;; Copyright (C) 2026 Free Software Foundation, Inc.

;; Author: Benchmark Suite
;; Keywords: benchmark, rendering, skia, cairo

;;; Commentary:

;; Comprehensive benchmark suite for comparing rendering performance
;; between Cairo (CPU), Skia Raster (CPU), and Skia GL (GPU) backends.
;;
;; Usage:
;;   emacs -Q -l benchmark/emacs-render-benchmark.el \
;;     --eval "(benchmark-run-all \"results.csv\")"
;;
;; Or interactively:
;;   M-x benchmark-run-all RET results.csv RET

;;; Code:

(require 'cl-lib)

;;; ============================================================
;;; Configuration
;;; ============================================================

(defgroup render-benchmark nil
  "Rendering benchmark configuration."
  :group 'development)

(defcustom benchmark-warmup-iterations 3
  "Number of warmup iterations before measurement."
  :type 'integer
  :group 'render-benchmark)

(defcustom benchmark-measurement-iterations 10
  "Number of measurement iterations per test."
  :type 'integer
  :group 'render-benchmark)

(defcustom benchmark-text-lines 10000
  "Number of lines for text rendering benchmarks."
  :type 'integer
  :group 'render-benchmark)

(defcustom benchmark-scroll-iterations 500
  "Number of scroll operations per benchmark."
  :type 'integer
  :group 'render-benchmark)

(defcustom benchmark-redraw-iterations 100
  "Number of redraw operations per benchmark."
  :type 'integer
  :group 'render-benchmark)

;;; ============================================================
;;; Backend Detection
;;; ============================================================

(defun benchmark-detect-backend ()
  "Detect the current rendering backend.
Returns one of: \"skia-gl\", \"skia-raster\", \"cairo\", or \"unknown\"."
  (cond
   ;; Check for Skia by looking for Skia-specific function
   ((fboundp 'pgtk-skia-gl-enabled-p)
    ;; pgtk-skia-gl-enabled-p requires a frame, so check if GL is likely
    ;; based on whether the function exists (it only exists in GL builds)
    (condition-case nil
        (if (and (display-graphic-p)
                 (pgtk-skia-gl-enabled-p))
            "skia-gl"
          "skia-raster")
      (error "skia-gl")))  ; Assume GL if we can't check (batch mode)
   ;; Check system-configuration-features as fallback
   ((string-match-p "SKIA" (or system-configuration-features ""))
    "skia-raster")
   ;; Cairo backend (PGTK without Skia, or explicit CAIRO)
   ((or (string-match-p "CAIRO" (or system-configuration-features ""))
        (string-match-p "PGTK" (or system-configuration-features "")))
    "cairo")
   ;; Unknown
   (t "unknown")))

(defun benchmark-backend-info ()
  "Return a plist with backend information."
  (list :backend (benchmark-detect-backend)
        :emacs-version emacs-version
        :system-type (symbol-name system-type)
        :window-system (symbol-name (framep (selected-frame)))
        :features system-configuration-features))

;;; ============================================================
;;; Timing Utilities
;;; ============================================================

(defun benchmark--current-time-ms ()
  "Return current time in milliseconds as a float."
  (let ((time (current-time)))
    (+ (* (float-time time) 1000.0))))

(defmacro benchmark--measure (&rest body)
  "Measure execution time of BODY in milliseconds.
Returns a plist with :time-ms, :gc-count, and :gc-time-ms."
  (declare (indent 0))
  `(progn
     ;; Force any pending GC before measurement
     (garbage-collect)
     (let ((gc-elapsed-start gc-elapsed)
           (gc-count-start gcs-done)
           (start-time (benchmark--current-time-ms)))
       ,@body
       ;; Force display
       (redisplay t)
       (let ((end-time (benchmark--current-time-ms)))
         (list :time-ms (- end-time start-time)
               :gc-count (- gcs-done gc-count-start)
               :gc-time-ms (* (- gc-elapsed gc-elapsed-start) 1000.0))))))

(defun benchmark--run-with-warmup (name warmups iterations func)
  "Run FUNC with WARMUPS warmup runs, then ITERATIONS measured runs.
NAME is the benchmark name for logging.
Returns a list of measurement plists."
  (message "Benchmark: %s (warming up %d iterations)" name warmups)
  ;; Warmup runs
  (dotimes (_ warmups)
    (funcall func)
    (garbage-collect))
  (garbage-collect)

  (message "Benchmark: %s (measuring %d iterations)" name iterations)
  ;; Measured runs
  (let ((results '()))
    (dotimes (i iterations)
      (let ((result (benchmark--measure (funcall func))))
        (push (cons (1+ i) result) results)
        (message "  Iteration %d: %.2f ms" (1+ i) (plist-get result :time-ms))))
    (nreverse results)))

;;; ============================================================
;;; Test Data Generation
;;; ============================================================

(defun benchmark--generate-code-text (lines)
  "Generate LINES of realistic code-like text."
  (with-temp-buffer
    (dotimes (i lines)
      (let ((indent (make-string (* 2 (mod i 5)) ?\s))
            (line-type (mod i 7)))
        (insert indent)
        (cond
         ((= line-type 0)
          (insert (format "function process_%d(data, options) {\n" i)))
         ((= line-type 1)
          (insert (format "  const result = transform(data, %d);\n" i)))
         ((= line-type 2)
          (insert (format "  if (result.valid && options.check) {\n")))
         ((= line-type 3)
          (insert (format "    return validate(result, \"%d\");\n" i)))
         ((= line-type 4)
          (insert "  }\n"))
         ((= line-type 5)
          (insert (format "  // Process iteration %d\n" i)))
         ((= line-type 6)
          (insert "}\n\n")))))
    (buffer-string)))

(defun benchmark--generate-unicode-text (lines)
  "Generate LINES of mixed Unicode text (emoji, CJK, Arabic, etc.)."
  (with-temp-buffer
    (dotimes (i lines)
      (let ((line-type (mod i 6)))
        (cond
         ;; Emoji-heavy line
         ((= line-type 0)
          (insert (format "Status %d: Complete! Check the results below.\n" i)))
         ;; CJK characters
         ((= line-type 1)
          (insert (format "%d: " i)))
         ;; Arabic (RTL)
         ((= line-type 2)
          (insert (format "Line %d: Test text with numbers 123\n" i)))
         ;; Mixed scripts
         ((= line-type 3)
          (insert (format "Mix %d: Hello World Test\n" i)))
         ;; Combining characters
         ((= line-type 4)
          (insert (format "Combining %d: cafe\n" i)))
         ;; Mathematical symbols
         ((= line-type 5)
          (insert (format "Math %d: Sum over i from 1 to n = n(n+1)/2\n" i))))))
    (buffer-string)))

;;; ============================================================
;;; Individual Benchmark Tests
;;; ============================================================

(defun benchmark-test--text-rendering ()
  "Benchmark: Render a buffer with many lines of code."
  (let ((text (benchmark--generate-code-text benchmark-text-lines)))
    (with-temp-buffer
      (insert text)
      (goto-char (point-min))
      (set-window-buffer (selected-window) (current-buffer))
      (redisplay t)
      ;; Scroll through the buffer to force rendering of different regions
      (dotimes (_ 10)
        (goto-char (random (point-max)))
        (redisplay t)))))

(defun benchmark-test--text-unicode ()
  "Benchmark: Render Unicode-heavy text with mixed scripts."
  (let ((text (benchmark--generate-unicode-text benchmark-text-lines)))
    (with-temp-buffer
      (insert text)
      (goto-char (point-min))
      (set-window-buffer (selected-window) (current-buffer))
      (redisplay t)
      ;; Scroll through
      (dotimes (_ 10)
        (goto-char (random (point-max)))
        (redisplay t)))))

(defun benchmark-test--text-font-changes ()
  "Benchmark: Text rendering with font attribute changes."
  (with-temp-buffer
    ;; Insert text with varying faces
    (dotimes (i 1000)
      (let ((face (nth (mod i 5)
                       '(default bold italic underline highlight))))
        (insert (propertize (format "Line %d with face %s\n" i face)
                            'face face))))
    (goto-char (point-min))
    (set-window-buffer (selected-window) (current-buffer))
    (redisplay t)
    ;; Scroll through
    (dotimes (_ 20)
      (forward-line 50)
      (redisplay t))))

(defun benchmark-test--scroll-line ()
  "Benchmark: Line-by-line scrolling in a large buffer."
  (with-temp-buffer
    (insert (benchmark--generate-code-text (* benchmark-text-lines 10)))
    (goto-char (point-min))
    (set-window-buffer (selected-window) (current-buffer))
    (redisplay t)
    ;; Scroll down line by line
    (dotimes (_ benchmark-scroll-iterations)
      (ignore-errors (scroll-up 1))
      (redisplay t))))

(defun benchmark-test--scroll-page ()
  "Benchmark: Page-by-page scrolling in a large buffer."
  (with-temp-buffer
    (insert (benchmark--generate-code-text (* benchmark-text-lines 10)))
    (goto-char (point-min))
    (set-window-buffer (selected-window) (current-buffer))
    (redisplay t)
    ;; Scroll down page by page
    (dotimes (_ (/ benchmark-scroll-iterations 10))
      (ignore-errors (scroll-up))
      (redisplay t))))

(defun benchmark-test--scroll-random ()
  "Benchmark: Random position jumping (simulates navigation)."
  (with-temp-buffer
    (insert (benchmark--generate-code-text (* benchmark-text-lines 10)))
    (set-window-buffer (selected-window) (current-buffer))
    (redisplay t)
    ;; Jump to random positions
    (dotimes (_ benchmark-scroll-iterations)
      (goto-char (1+ (random (1- (point-max)))))
      (recenter)
      (redisplay t))))

(defun benchmark-test--images-display ()
  "Benchmark: Display multiple images."
  ;; Create test images programmatically using XPM format
  (with-temp-buffer
    (dotimes (i 20)
      (let* ((size (+ 50 (* i 10)))
             (color (nth (mod i 6) '("red" "green" "blue" "yellow" "cyan" "magenta")))
             ;; Create a simple XPM image
             (xpm-data (format "/* XPM */
static char *img[] = {
\"%d %d 2 1\",
\"  c None\",
\"X c %s\",
%s};"
                               size size color
                               (mapconcat
                                (lambda (_)
                                  (concat "\""
                                          (make-string size ?X)
                                          "\""))
                                (number-sequence 1 size)
                                ",\n")))
             (img (create-image xpm-data 'xpm t)))
        (insert-image img)
        (insert "  ")))
    (insert "\n\n")
    (goto-char (point-min))
    (set-window-buffer (selected-window) (current-buffer))
    ;; Force multiple redraws
    (dotimes (_ 20)
      (redisplay t))))

(defun benchmark-test--images-scroll ()
  "Benchmark: Scroll through image-heavy buffer."
  (with-temp-buffer
    ;; Create many small images
    (dotimes (_row 50)
      (dotimes (_col 10)
        (let* ((size 30)
               (xpm-data (format "/* XPM */
static char *img[] = {
\"%d %d 2 1\",
\"  c None\",
\"X c #%02x%02x%02x\",
%s};"
                                 size size
                                 (random 256) (random 256) (random 256)
                                 (mapconcat
                                  (lambda (_)
                                    (concat "\"" (make-string size ?X) "\""))
                                  (number-sequence 1 size)
                                  ",\n")))
               (img (create-image xpm-data 'xpm t)))
          (insert-image img)
          (insert " ")))
      (insert "\n"))
    (goto-char (point-min))
    (set-window-buffer (selected-window) (current-buffer))
    (redisplay t)
    ;; Scroll through
    (dotimes (_ 100)
      (ignore-errors (scroll-up 2))
      (redisplay t))))

(defun benchmark-test--redraw-force ()
  "Benchmark: Force full window redraws."
  (with-temp-buffer
    (insert (benchmark--generate-code-text 1000))
    (goto-char (point-min))
    (set-window-buffer (selected-window) (current-buffer))
    (dotimes (_ benchmark-redraw-iterations)
      (force-window-update)
      (redisplay t))))

(defun benchmark-test--redraw-with-colors ()
  "Benchmark: Redraw with face/color changes."
  (with-temp-buffer
    (insert (benchmark--generate-code-text 1000))
    (goto-char (point-min))
    (set-window-buffer (selected-window) (current-buffer))
    (dotimes (i benchmark-redraw-iterations)
      ;; Change background color
      (set-face-background 'default
                           (format "#%02x%02x%02x"
                                   (+ 240 (mod i 15))
                                   (+ 240 (mod i 15))
                                   (+ 240 (mod i 15))))
      (force-window-update)
      (redisplay t))
    ;; Reset
    (set-face-background 'default nil)))

(defun benchmark-test--resize-frame ()
  "Benchmark: Frame resize operations."
  (let ((frame (selected-frame))
        (orig-width (frame-width))
        (orig-height (frame-height)))
    (unwind-protect
        (dotimes (i 50)
          (let ((new-width (+ 80 (mod (* i 3) 40)))
                (new-height (+ 24 (mod (* i 2) 20))))
            (set-frame-size frame new-width new-height)
            (redisplay t)))
      ;; Restore original size
      (set-frame-size frame orig-width orig-height))))

;;; ============================================================
;;; Benchmark Registry
;;; ============================================================

(defvar benchmark-tests
  '(("text-code" . benchmark-test--text-rendering)
    ("text-unicode" . benchmark-test--text-unicode)
    ("text-faces" . benchmark-test--text-font-changes)
    ("scroll-line" . benchmark-test--scroll-line)
    ("scroll-page" . benchmark-test--scroll-page)
    ("scroll-random" . benchmark-test--scroll-random)
    ("images-display" . benchmark-test--images-display)
    ("images-scroll" . benchmark-test--images-scroll)
    ("redraw-force" . benchmark-test--redraw-force)
    ("redraw-colors" . benchmark-test--redraw-with-colors)
    ("resize-frame" . benchmark-test--resize-frame))
  "Alist of (NAME . FUNCTION) for all benchmark tests.")

;;; ============================================================
;;; Results Output
;;; ============================================================

(defun benchmark--results-to-csv (backend results)
  "Convert RESULTS to CSV format with BACKEND label.
RESULTS is an alist of (test-name . measurements)."
  (with-temp-buffer
    ;; Header
    (insert "backend,test,iteration,time_ms,gc_count,gc_time_ms\n")
    ;; Data rows
    (dolist (test-result results)
      (let ((test-name (car test-result))
            (measurements (cdr test-result)))
        (dolist (m measurements)
          (let ((iter (car m))
                (data (cdr m)))
            (insert (format "%s,%s,%d,%.3f,%d,%.3f\n"
                            backend
                            test-name
                            iter
                            (plist-get data :time-ms)
                            (plist-get data :gc-count)
                            (plist-get data :gc-time-ms)))))))
    (buffer-string)))

(defun benchmark--write-csv (filename content)
  "Write CONTENT to FILENAME."
  (with-temp-file filename
    (insert content))
  (message "Results written to: %s" filename))

;;; ============================================================
;;; Main Entry Points
;;; ============================================================

(defun benchmark-run-test (test-name)
  "Run a single benchmark TEST-NAME and return results."
  (interactive
   (list (completing-read "Benchmark: "
                          (mapcar #'car benchmark-tests)
                          nil t)))
  (let* ((func (cdr (assoc test-name benchmark-tests)))
         (results (benchmark--run-with-warmup
                   test-name
                   benchmark-warmup-iterations
                   benchmark-measurement-iterations
                   func)))
    (message "\nResults for %s:" test-name)
    (let ((times (mapcar (lambda (r) (plist-get (cdr r) :time-ms)) results)))
      (message "  Min: %.2f ms" (apply #'min times))
      (message "  Max: %.2f ms" (apply #'max times))
      (message "  Avg: %.2f ms" (/ (apply #'+ times) (float (length times)))))
    results))

(defun benchmark-run-all (output-file)
  "Run all benchmarks and write results to OUTPUT-FILE."
  (interactive "FOutput CSV file: ")
  (let* ((backend (benchmark-detect-backend))
         (all-results '()))
    (message "")
    (message "========================================")
    (message "Emacs Rendering Benchmark Suite")
    (message "========================================")
    (message "Backend: %s" backend)
    (message "Emacs: %s" emacs-version)
    (message "Warmup iterations: %d" benchmark-warmup-iterations)
    (message "Measurement iterations: %d" benchmark-measurement-iterations)
    (message "========================================")
    (message "")

    ;; Run each test
    (dolist (test benchmark-tests)
      (let* ((test-name (car test))
             (test-func (cdr test))
             (results (benchmark--run-with-warmup
                       test-name
                       benchmark-warmup-iterations
                       benchmark-measurement-iterations
                       test-func)))
        (push (cons test-name results) all-results)
        ;; Brief pause between tests
        (garbage-collect)
        (sleep-for 0.5)))

    ;; Write results
    (setq all-results (nreverse all-results))
    (benchmark--write-csv output-file
                          (benchmark--results-to-csv backend all-results))

    ;; Print summary
    (message "")
    (message "========================================")
    (message "Summary")
    (message "========================================")
    (dolist (test-result all-results)
      (let* ((test-name (car test-result))
             (measurements (cdr test-result))
             (times (mapcar (lambda (r) (plist-get (cdr r) :time-ms))
                            measurements))
             (avg (/ (apply #'+ times) (float (length times)))))
        (message "  %-20s avg: %8.2f ms" test-name avg)))
    (message "========================================")
    (message "Results saved to: %s" output-file)

    all-results))

(defun benchmark-quick ()
  "Run a quick benchmark with fewer iterations for testing."
  (interactive)
  (let ((benchmark-warmup-iterations 1)
        (benchmark-measurement-iterations 3)
        (benchmark-text-lines 1000)
        (benchmark-scroll-iterations 50)
        (benchmark-redraw-iterations 20))
    (benchmark-run-all
     (expand-file-name
      (format "benchmark-quick-%s.csv" (benchmark-detect-backend))
      (or (getenv "HOME") "/tmp")))))

;;; ============================================================
;;; Batch Mode Support
;;; ============================================================

(defun benchmark-batch-run ()
  "Run benchmarks in batch mode.
Usage: emacs --batch -l benchmark/emacs-render-benchmark.el \\
       -f benchmark-batch-run"
  (let ((output-file (or (getenv "BENCHMARK_OUTPUT")
                         (format "benchmark-%s-%s.csv"
                                 (benchmark-detect-backend)
                                 (format-time-string "%Y%m%d-%H%M%S")))))
    (benchmark-run-all output-file)
    (kill-emacs 0)))

(provide 'emacs-render-benchmark)

;;; emacs-render-benchmark.el ends here
