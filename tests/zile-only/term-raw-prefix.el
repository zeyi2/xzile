; In a terminal in char mode, C-u is not a prefix argument but goes to
; the program: here it erases the line typed so far.
(term "/bin/sh")
(sit-for 1)
(execute-kbd-macro "echo wrong\C-uecho raw-ok >term-raw-prefix.tmp; exit\r")
(sit-for 2)
(switch-to-buffer "term-raw-prefix.input")
(shell-command "cat term-raw-prefix.tmp; rm -f term-raw-prefix.tmp" t)
(save-buffer)
(save-buffers-kill-emacs)
