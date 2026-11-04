; previous-buffer and next-buffer move through the window's history,
; with bare LEFT and RIGHT continuing; killing a buffer shows the one
; before it; going forward restores point.
(execute-kbd-macro "\C-xbone\r1\C-xbtwo\r2\C-xbthree\r3\C-x\LEFT\LEFT\RIGHT\C-g\C-xh\M-w\C-xbwinhist.input\r\M-<\C-y\C-x\LEFT\C-xk\r\C-xh\M-w\C-x\RIGHT\C-y\C-x\C-s\C-x\C-c")
