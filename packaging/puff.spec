Name:           puff
Version:        0.2.0
Release:        2%{?dist}
Summary:        Native GTK database workspace for PostgreSQL and SQLite
License:        MIT
Source0:        %{name}-%{version}.tar.gz
%if 0%{?puff_prebuilt}
Source1:        puff
%endif
%global debug_package %{nil}
%if !0%{?puff_prebuilt}
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  pkgconfig(gtk4) >= 4.12
BuildRequires:  pkgconfig(libadwaita-1) >= 1.4
BuildRequires:  pkgconfig(gtksourceview-5)
BuildRequires:  pkgconfig(libpq) >= 17
BuildRequires:  pkgconfig(sqlite3)
BuildRequires:  pkgconfig(libsecret-1)
%endif
BuildRequires:  desktop-file-utils
Requires:       hicolor-icon-theme
Requires:       gtk4 >= 4.12
Requires:       libadwaita >= 1.4
Requires:       gtksourceview5
Requires:       libpq >= 17
Requires:       sqlite-libs
Requires:       libsecret
Requires:       libstdc++
Recommends:     postgresql

%description
My Puff is a compiled C++20 database workspace using native GTK4, libadwaita
and GtkSourceView widgets. It includes PostgreSQL and SQLite drivers,
incremental query results, safe row editing, schema management and native
PostgreSQL backup utilities. No web renderer or JavaScript runtime is bundled.

%prep
%setup -q

%build
%if !0%{?puff_prebuilt}
%make_build
%endif

%install
%if 0%{?puff_prebuilt}
install -Dm755 %{SOURCE1} %{buildroot}%{_bindir}/puff
%else
install -Dm755 dist/puff %{buildroot}%{_bindir}/puff
%endif
install -Dm644 packaging/puff.desktop %{buildroot}%{_datadir}/applications/io.github.puff.Puff.desktop
install -Dm644 packaging/puff.svg %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/puff.svg
install -Dm644 packaging/io.github.puff.Puff.metainfo.xml %{buildroot}%{_datadir}/metainfo/io.github.puff.Puff.metainfo.xml

%check
desktop-file-validate packaging/puff.desktop

%files
%license LICENSE
%doc README.md docs
%{_bindir}/puff
%{_datadir}/applications/io.github.puff.Puff.desktop
%{_datadir}/icons/hicolor/scalable/apps/puff.svg
%{_datadir}/metainfo/io.github.puff.Puff.metainfo.xml

%changelog
* Sun Oct 18 2026 Puff contributors - 0.2.0-1
- Compiled C++20 GTK4/libadwaita application and native database drivers
