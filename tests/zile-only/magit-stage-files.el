; Stage files and sections, unstage, and discard from magit-status.
(shell-command "rm -rf magit-tmp2 && mkdir magit-tmp2 && cd magit-tmp2 && git init -q -b main && git config user.email t@t && git config user.name T && git config commit.gpgsign false && echo a > a.txt && echo b > b.txt && echo c > c.txt && git add . && git commit -qm init && echo A >> a.txt && echo B >> b.txt && echo x > x.txt && echo y > y.txt")
(magit-status "magit-tmp2")
; On the "Untracked files" heading, s stages both untracked files;
; on "modified a.txt", s stages it; on "Staged changes", u unstages all,
; then s on "x.txt" stages it again; k y on "modified b.txt" discards it.
(execute-kbd-macro "\C-sUntracked\rs\M-<\C-smodified   a\rs\M-<\C-sStaged changes\ru\M-<\C-sx.txt\rs\M-<\C-smodified   b\rky")
(switch-to-buffer "magit-stage-files.input")
(shell-command "cd magit-tmp2 && git status --short; cd .. && rm -rf magit-tmp2" t)
(save-buffer)
(save-buffers-kill-emacs)
