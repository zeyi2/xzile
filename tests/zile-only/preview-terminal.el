; Previews need xzile-gui: in the terminal they fail, leaving the text
; as it was.
(image-mode)
(pdf-mode)
(preview-toggle-display)
(preview-next-page)
(preview-zoom-in)
(preview-quit)
(goto-char (point-min))
(insert "Still text. ")
(save-buffer)
(save-buffers-kill-emacs)
