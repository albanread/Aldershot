;;;; bench.ss -- the language comparison in Scheme (Chez Scheme, !Scheme):
;;;; fib(30), a sieve of the primes below 2,000,000, 200,000 string keys in
;;;; a string hashtable; the best of three, in milliseconds.
(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))

(define (sieve)
  (let* ([n 2000000] [s (make-bytevector (+ n 1) 0)])
    (let loop ([i 2] [c 0])
      (cond
        [(> i n) c]
        [(= 0 (bytevector-u8-ref s i))
         (do ([j (* i i) (+ j i)]) ((> j n)) (bytevector-u8-set! s j 1))
         (loop (+ i 1) (+ c 1))]
        [else (loop (+ i 1) c)]))))

(define (key i) (string-append "k" (number->string i)))

(define (dict)
  (let ([d (make-hashtable string-hash string=?)])
    (do ([i 1 (+ i 1)]) ((> i 200000)) (hashtable-set! d (key i) (* i 2)))
    (do ([i 1 (+ i 1)] [s 0 (+ s (hashtable-ref d (key i) 0))]) ((> i 200000) s))))

;; the monotonic clock, in milliseconds (it counts nanoseconds)
(define (now-ms)
  (let ([t (current-time 'time-monotonic)])
    (+ (* 1000.0 (time-second t)) (/ (time-nanosecond t) 1e6))))

(define (best name f)
  (let loop ([k 0] [b #f] [r #f])
    (if (= k 3)
        (printf "scheme ~a ~a ~a~%" name
                (/ (round (* b 10)) 10.0) r)
        (let* ([t0 (now-ms)] [r (f)] [ms (- (now-ms) t0)])
          (loop (+ k 1) (if (or (not b) (< ms b)) ms b) r)))))

(best "fib" (lambda () (fib 30)))
(best "sieve" sieve)
(best "dict" dict)
