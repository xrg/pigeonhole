%define git_repo dovecot-pigeonhole
%define git_head HEAD

%define dovecot_version 2.2

Name:           dovecot-pigeonhole
Summary:        Pigeonhole Sieve/ManageSieve plugin for dovecot LDA
Group:          System/Servers
Version:        %git_get_ver
Release:        %mkrel %git_get_rel2
License:        MIT and LGPLv2 and BSD-like and Public Domain
URL:            http://dovecot.org
Source:         %git_bs_source %{name}-%{version}.tar.gz
Source1:        %{name}-gitrpm.version
Source2:        %{name}-changelog.gitrpm.txt
Provides:       imap-server pop3-server
Provides:       imaps-server pop3s-server
Requires:       dovecot = %{dovecot_version}
Obsoletes:      %{name}-plugins-sieve < 2.0, %{name}-plugins-managesieve < 2.0
Requires(post): systemd >= %{systemd_required_version}
BuildRequires:  dovecot-devel
BuildRequires:  gettext-devel


%description
This package provides the Pigeonhole Sieve/ManageSieve plugin version %{pigeonhole_ver}
for dovecot LDA.

%package devel
Summary:        Pigeonhole Sieve/ManageSieve development files
Group:          Development/C
Requires:       %{name}-pigeonhole >= %{version}

%description devel
This package contains development files for Pigeonhole Sieve/ManageSieve %{pigeonhole_ver}.

%prep
%git_get_source
%setup -q

%build
%serverbuild
./autogen.sh

autoreconf -fi
%configure2_5x \
    --disable-static \
    --with-dovecot=../ \
    --with-unfinished-features 
%make

%install
%makeinstall_std
mv %{buildroot}%{_libdir}/dovecot/sieve %{buildroot}%{_libdir}/dovecot/modules
install -m 644 doc/example-config/conf.d/*.conf* %{buildroot}%{_sysconfdir}/dovecot/conf.d
popd

install -d -m 755 %{buildroot}%{_docdir}/dovecot-pigeonhole

# procmail2sieve converter
install -d -m 755 %{buildroot}%{_bindir}
install contrib/mageia/procmail2sieve.pl -m 755 %{buildroot}%{_bindir}
perl -pi -e 's|#!/usr/local/bin/perl|#!%{_bindir}/perl|' \
    %{buildroot}%{_bindir}/procmail2sieve.pl


%files
%doc %{pigeonhole_dir}/{AUTHORS,ChangeLog,COPYING*,INSTALL,NEWS,README}
%doc %{pigeonhole_dir}/doc/*
%{_sysconfdir}/dovecot/conf.d/20-managesieve.conf
%{_sysconfdir}/dovecot/conf.d/90-sieve.conf
%{_sysconfdir}/dovecot/conf.d/90-sieve-extprograms.conf
%{_bindir}/procmail2sieve.pl
%{_bindir}/sieve-dump
%{_bindir}/sieve-filter
%{_bindir}/sieve-test
%{_bindir}/sievec
%{_libdir}/dovecot/libdovecot-sieve.so*
%{_libexecdir}/dovecot/managesieve
%{_libexecdir}/dovecot/managesieve-login
%{_libdir}/dovecot/modules/lib90_sieve_plugin.so
%dir %{_libdir}/dovecot/modules/sieve
%{_libdir}/dovecot/modules/sieve/lib90_sieve_extprograms_plugin.so
%{_libdir}/dovecot/modules/doveadm/lib10_doveadm_sieve_plugin.so
%{_libdir}/dovecot/modules/settings/libmanagesieve_settings.so
%{_libdir}/dovecot/modules/settings/libmanagesieve_login_settings.so
%{_mandir}/man1/sievec.1*
%{_mandir}/man1/sieved.1*
%{_mandir}/man1/sieve-dump.1*
%{_mandir}/man1/sieve-filter.1*
%{_mandir}/man1/sieve-test.1*
%{_mandir}/man7/pigeonhole.7*

%files devel
%{_includedir}/dovecot/sieve

%changelog -f %{_sourcedir}/%{name}-changelog.gitrpm.txt
