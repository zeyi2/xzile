; A Markdown file on another machine is in Markdown mode, whatever the
; case of its suffix: C-c C-a aligns its table, which is saved there.
(shell-command "printf '| a | bb |\n|-|-:|\n| ccc | d |\n' > $XZ_REMOTE_HOME/notes.MD")
; C-x C-f M-l xztest RET notes.MD RET: open it; C-c C-a C-x C-s: align
; the table on its first line and save it
(execute-kbd-macro "\C-x\C-f\M-lxztest\rnotes.MD\r\C-c\C-a\C-x\C-s")
(execute-kbd-macro "\C-xbremote-markdown.input\r")
(shell-command "cat $XZ_REMOTE_HOME/notes.MD" t)
(save-buffer)
(save-buffers-kill-emacs)
