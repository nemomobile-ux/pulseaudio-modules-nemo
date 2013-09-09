# 
# Do NOT Edit the Auto-generated Part!
# Generated by: spectacle version 0.26
# 

Name:       pulseaudio-modules-nemo

# >> macros
%define pulseversion 4.0
# << macros

Summary:    PulseAudio modules for Nemo
Version:    4.0.6
Release:    1
Group:      Multimedia/PulseAudio
License:    LGPLv2.1+
URL:        https://github.com/nemomobile/pulseaudio-modules-nemo
Source0:    %{name}-%{version}.tar.gz
Source100:  pulseaudio-modules-nemo.yaml
BuildRequires:  pkgconfig(alsa) >= 1.0.19
BuildRequires:  pkgconfig(check)
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(pulsecore) >= 4.0
BuildRequires:  libtool-ltdl-devel

%description
PulseAudio modules for Nemo.

%package common
Summary:    Common libs for the Nemo PulseAudio modules
Group:      Multimedia/PulseAudio
Requires:   pulseaudio >= 4.0
Obsoletes:  pulseaudio-modules-nemo-voice < 4.0.6
Obsoletes:  pulseaudio-modules-nemo-music < 4.0.6
Obsoletes:  pulseaudio-modules-nemo-record < 4.0.6
Obsoletes:  pulseaudio-modules-nemo-sidetone < 4.0.6

%description common
This contains common libs for the Nemo PulseAudio modules.

%package mainvolume
Summary:    Mainvolume module for PulseAudio
Group:      Multimedia/PulseAudio
Requires:   %{name}-common = %{version}-%{release}
Requires:   %{name}-stream-restore
Requires:   pulseaudio >= 4.0

%description mainvolume
This contains mainvolume module for PulseAudio

%package parameters
Summary:    Algorithm parameter manager module for PulseAudio
Group:      Multimedia/PulseAudio
Requires:   %{name}-common = %{version}-%{release}
Requires:   pulseaudio >= 4.0

%description parameters
This contains an algorithm parameter manager module for PulseAudio

%package test
Summary:    Test module for PulseAudio
Group:      Multimedia/PulseAudio
Requires:   %{name}-common = %{version}-%{release}
Requires:   pulseaudio >= 4.0

%description test
This contains a test module for PulseAudio

%package stream-restore
Summary:    Modified version of the original stream-restore module for PulseAudio
Group:      Multimedia/PulseAudio
Requires:   %{name}-common = %{version}-%{release}
Requires:   pulseaudio >= 4.0

%description stream-restore
This contains a modified version of the original stream-restore module in PulseAudio.

%package devel
Summary:    Development files for modules.
Group:      Development/Libraries
Requires:   %{name}-common = %{version}-%{release}
Requires:   pulseaudio >= 4.0

%description devel
This contains development files for nemo modules.

%prep
%setup -q -n %{name}-%{version}

# >> setup
# << setup

%build
# >> build pre
autoreconf -vfi
# << build pre

%reconfigure --disable-static
make %{?jobs:-j%jobs}

# >> build post
# << build post

%install
rm -rf %{buildroot}
# >> install pre
# << install pre
%make_install

# >> install post
install -d %{buildroot}/%{_prefix}/include/pulsecore/modules/meego
install -m 644 src/common/include/meego/*.h %{buildroot}/%{_prefix}/include/pulsecore/modules/meego
install -d %{buildroot}/%{_libdir}/pkgconfig
install -m 644 src/common/*.pc %{buildroot}/%{_libdir}/pkgconfig
# << install post


%files common
%defattr(-,root,root,-)
# >> files common
%{_libdir}/pulse-%{pulseversion}/modules/libmeego-common.so
# << files common

%files mainvolume
%defattr(-,root,root,-)
# >> files mainvolume
%{_libdir}/pulse-%{pulseversion}/modules/module-meego-mainvolume.so
# << files mainvolume

%files parameters
%defattr(-,root,root,-)
# >> files parameters
%{_libdir}/pulse-%{pulseversion}/modules/module-meego-parameters.so
# << files parameters

%files test
%defattr(-,root,root,-)
# >> files test
%{_libdir}/pulse-%{pulseversion}/modules/module-meego-test.so
# << files test

%files stream-restore
%defattr(-,root,root,-)
# >> files stream-restore
%{_libdir}/pulse-%{pulseversion}/modules/module-stream-restore-nemo.so
# << files stream-restore

%files devel
%defattr(-,root,root,-)
# >> files devel
%{_prefix}/include/pulsecore/modules/meego/*.h
%{_libdir}/pkgconfig/*.pc
# << files devel