; Edited *compilation* output is read as it now is: once the line
; entering a directory is replaced, an error no longer names the file
; of that name there, but the one next to the file compiled.
; The other file is in a directory of its own, whose name is kept next
; to the file compiled (as the input files are, so ignored by git) for
; removing it after, with a backup that a failure may have saved.
(compile "d=$(mktemp -d) && echo $d > compile-edited-dirs.tmpdir.input && echo other > $d/compile-edited-dirs.input && printf 'make: Entering directory \047%s\047\n' $d && echo compile-edited-dirs.input:2:1: error: e")
(sit-for 2)
(next-error)
(switch-to-buffer "*compilation*")
(toggle-read-only)
(beginning-of-buffer)
; The first is in the command echoed.
(search-forward "Entering")
(search-forward "Entering")
(beginning-of-line)
(kill-line)
(insert "edited")
(search-forward "error:")
(compile-goto-error)
(insert "A")
(save-buffer)
; Run in the directory of the first compilation, whichever file is current.
(switch-to-buffer "*compilation*")
(compile "d=$(cat compile-edited-dirs.tmpdir.input) && rm -f $d/compile-edited-dirs.input $d/compile-edited-dirs.input~ && rmdir $d && rm -f compile-edited-dirs.tmpdir.input")
(sit-for 1)
(save-buffers-kill-emacs)
