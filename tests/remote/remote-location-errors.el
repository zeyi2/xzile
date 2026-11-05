; Machines that cannot be used are not chosen: the location list comes
; back, saying why. C-g there returns to the local file name, kept as
; it was, and C-g during a connection cancels it.
; C-x C-f M-l: xzbadkey (key refused), xznohostkey (unknown host key),
; 127.0.0.1:1 (nothing listens), xztest C-g (cancelled); C-g: back to
; the local name; the test file's name RET: visits it again
(execute-kbd-macro "\C-x\C-f\M-lxzbadkey\rxznohostkey\r127.0.0.1:1\rxztest\r\C-g\C-gremote-location-errors.input\r")
(execute-kbd-macro "\M-<still local\r\C-x\C-s\C-x\C-c")
