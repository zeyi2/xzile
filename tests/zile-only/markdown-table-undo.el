; C-c C-a aligns the table in one change, which one undo takes back; a
; second C-c C-a, on the table aligned, changes nothing, so the undo
; is of the first. (Aligning twice once hung, from text left in the
; buffer's gap.)
(markdown-mode)
(execute-kbd-macro "\C-n\C-n\C-c\C-a\C-c\C-a\C-xu\C-x\C-s\C-x\C-c")
