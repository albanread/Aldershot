;;;; bench.lisp -- the language comparison in Lisp (NCL): fib(30), a sieve
;;;; of the primes below 2,000,000, 200,000 string keys in an EQUAL hash
;;;; table; the best of three, in milliseconds.
(defun fib (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))

(defun sieve ()
  (let* ((n 2000000) (s (make-array (+ n 1) :initial-element 0)) (c 0))
    (loop for i from 2 to n
          do (when (= 0 (aref s i))
               (setq c (+ c 1))
               (loop for j from (* i i) to n by i do (setf (aref s j) 1))))
    c))

(defun dict ()
  (let ((d (make-hash-table :test #'equal)) (s 0))
    (loop for i from 1 to 200000 do (setf (gethash (format nil "k~D" i) d) (* i 2)))
    (loop for i from 1 to 200000 do (setq s (+ s (gethash (format nil "k~D" i) d))))
    s))

;; get-internal-real-time counts nanoseconds
(defun best (name f)
  (let ((b nil) (r nil))
    (dotimes (k 3)
      (let ((t0 (get-internal-real-time)))
        (setq r (funcall f))
        (let ((ms (/ (- (get-internal-real-time) t0) 1000000.0)))
          (when (or (null b) (< ms b)) (setq b ms)))))
    (format t "lisp ~A ~,1F ~A~%" name b r)))

(best "fib" (lambda () (fib 30)))
(best "sieve" #'sieve)
(best "dict" #'dict)
