; Run a command in a terminal buffer.
(term "/bin/sh")
(sit-for 1)
(execute-kbd-macro "echo term-ok >term-shell.tmp; exit\r")
(sit-for 2)
(switch-to-buffer "term-shell.input")
(shell-command "cat term-shell.tmp; rm -f term-shell.tmp" t)
(save-buffer)
(save-buffers-kill-emacs)
