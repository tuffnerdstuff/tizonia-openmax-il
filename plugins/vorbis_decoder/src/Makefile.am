# Copyright (C) 2011-2019 Aratelia Limited - Juan A. Rubio
#
# This file is part of Tizonia
#
# Tizonia is free software: you can redistribute it and/or modify it under the
# terms of the GNU Lesser General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option)
# any later version.
#
# Tizonia is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
# more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with Tizonia.  If not, see <http://www.gnu.org/licenses/>.

libtizvorbisddir = $(plugindir)

libtizvorbisd_LTLIBRARIES = libtizvorbisd.la

noinst_HEADERS = \
	vorbisd.h \
	vorbisdprc.h \
	vorbisdprc_decls.h

libtizvorbisd_la_SOURCES = \
	vorbisd.c \
	vorbisdprc.c

libtizvorbisd_la_CFLAGS = \
	@TIZILHEADERS_CFLAGS@ \
	@TIZPLATFORM_CFLAGS@ \
	@TIZONIA_CFLAGS@ \
	@FISHSOUND_CFLAGS@

libtizvorbisd_la_LDFLAGS = -version-info @SHARED_VERSION_INFO@ @SHLIB_VERSION_ARG@

libtizvorbisd_la_LIBADD = \
	@TIZPLATFORM_LIBS@ \
	@TIZONIA_LIBS@ \
	@FISHSOUND_LIBS@


