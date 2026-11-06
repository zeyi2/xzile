; C-c C-s b, C-c C-s i and C-c C-s c wrap the region in **, * or `, or
; unwrap it if it is wrapped already, inside or out; with no region
; they insert a pair with point inside. C-c C-x C-x checks a task, or
; unchecks it.
(markdown-mode)
(execute-kbd-macro "\M-f\M-f\C-f\C-@\M-f\C-c\C-sb\C-n\C-a\M-f\M-f\C-f\C-@\M-f\C-c\C-si\C-n\C-a\M-f\C-f\C-@\M-f\C-c\C-sc\C-n\C-a\C-f\C-f\C-@\C-f\C-f\C-f\C-f\C-f\C-f\C-c\C-sc\C-n\C-a\C-f\C-f\C-@\M-f\C-c\C-sb\C-n\C-c\C-x\C-x\C-n\C-c\C-x\C-x\C-n\C-e \C-c\C-sb!\C-x\C-s\C-x\C-c")
