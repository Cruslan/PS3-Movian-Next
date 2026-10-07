CGC ?= $(shell which cgc 2>/dev/null || echo /opt/nvidia-cg-toolkit/bin/cgc)
CGCOMP ?= $(shell which cgcomp 2>/dev/null || echo $(CURDIR)/../../ps3dev/bin/cgcomp)

all: 	v1.vp \
	yuv2rgb_v.vp \
	f_tex.fp \
	f_flat.fp \
	f_tex_blur.fp \
	yuv2rgb_1f_norm.fp \
	yuv2rgb_2f_norm.fp \
	f_flat_stencil.fp \
	f_tex_stencil.fp \
	f_tex_stencil_blur.fp

%.fp40: ../glsl/%.glsl
	$(CGC) -oglsl -profile fp40 $< >$@

%.fp: %.fp40
	$(CGCOMP) -a -f $< $@
	@gio trash $< 2>/dev/null || true

%.vp40: ../glsl/%.glsl
	$(CGC) -oglsl -profile vp40 $< >$@

%.vp: %.vp40
	$(CGCOMP) -a -v $< $@
	@gio trash $< 2>/dev/null || true

clean:
	@for f in *.fp *.vp *.fp40 *.vp40; do \
		if [ -e "$$f" ]; then \
			gio trash "$$f" 2>/dev/null || true; \
		fi; \
	done

.PHONY: all clean
