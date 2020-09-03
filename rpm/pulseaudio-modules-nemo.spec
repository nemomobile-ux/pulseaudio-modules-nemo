%define pulseversion %{expand:%(rpm -q --qf '[%%{version}]' pulseaudio)}
%define pulsemajorminor %{expand:%(echo '%{pulseversion}' | cut -d+ -f1)}
%define moduleversion %{pulsemajorminor}.%{expand:%(echo '%{version}' | awk -F. '{print $NF}')}

Name:       pulseaudio-modules-nemo

Summary:    PulseAudio modules for Nemo
Version:    %{pulsemajorminor}.30
Release:    1
Group:      Multimedia/PulseAudio
License:    LGPLv2+
URL:        https://git.sailfishos.org/mer-core/pulseaudio-modules-nemo
Source0:    %{name}-%{version}.tar.gz
BuildRequires:  pkgconfig(alsa) >= 1.0.19
BuildRequires:  pkgconfig(check)
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(libpulse)
BuildRequires:  libtool-ltdl-devel

%description
PulseAudio modules for Nemo.

%package common
Summary:    Common libs for the Nemo PulseAudio modules
Requires:   pulseaudio >= %{pulseversion}
Obsoletes:  pulseaudio-modules-nemo-voice < 4.0.6
Obsoletes:  pulseaudio-modules-nemo-music < 4.0.6
Obsoletes:  pulseaudio-modules-nemo-record < 4.0.6
Obsoletes:  pulseaudio-modules-nemo-sidetone < 4.0.6

%description common
This contains common libs for the Nemo PulseAudio modules.

%package music
Summary:    Music module for PulseAudio
Requires:   %{name}-common = %{version}-%{release}
Requires:   pulseaudio >= %{pulseversion}

%description music
This contains music module for PulseAudio

%package record
Summary:    Cmtspeech module for PulseAudio
Requires:   %{name}-common = %{version}-%{release}
Requires:   pulseaudio >= %{pulseversion}

%description record
This contains record module for PulseAudio

%package voice
Summary:    Voice module for PulseAudio
Requires:   %{name}-common = %{version}-%{release}
Requires:   pulseaudio >= %{pulseversion}

%description voice
This contains voice module for PulseAudio

%package mainvolume
Summary:    Mainvolume module for PulseAudio
Requires:   %{name}-common = %{version}-%{release}
Requires:   %{name}-stream-restore
Requires:   pulseaudio >= %{pulseversion}

%description mainvolume
This contains mainvolume module for PulseAudio

%package parameters
Summary:    Algorithm parameter manager module for PulseAudio
Requires:   %{name}-common = %{version}-%{release}
Requires:   pulseaudio >= %{pulseversion}

%description parameters
This contains an algorithm parameter manager module for PulseAudio

%package sidetone
Summary:    Sidetone module for PulseAudio
Requires:   %{name}-common = %{version}-%{release}
Requires:   pulseaudio >= %{pulseversion}

%description sidetone
This contains a sidetone module for PulseAudio

%package test
Summary:    Test module for PulseAudio
Requires:   %{name}-common = %{version}-%{release}
Requires:   pulseaudio >= %{pulseversion}

%description test
This contains a test module for PulseAudio

%package stream-restore
Summary:    Modified version of the original stream-restore module for PulseAudio
Requires:   %{name}-common = %{version}-%{release}
Requires:   pulseaudio >= %{pulseversion}

%description stream-restore
This contains a modified version of the original stream-restore module in PulseAudio.

%package devel
Summary:    Development files for modules.
Group:      Development/Libraries
Requires:   %{name}-common = %{version}-%{release}
Requires:   pulseaudio >= %{pulseversion}

%description devel
This contains development files for nemo modules.

%prep
%setup -q -n %{name}-%{version}


%build
echo "%{moduleversion}" > .tarball-version
%reconfigure --disable-static
make %{?jobs:-j%jobs}


%install
rm -rf %{buildroot}
%make_install

install -d %{buildroot}/%{_prefix}/include/pulsecore/modules/meego
install -m 644 src/common/include/meego/*.h %{buildroot}/%{_prefix}/include/pulsecore/modules/meego
install -m 644 src/voice/module-voice-api.h %{buildroot}/%{_prefix}/include/pulsecore/modules/meego
install -m 644 src/music/module-music-api.h %{buildroot}/%{_prefix}/include/pulsecore/modules/meego
install -m 644 src/record/module-record-api.h %{buildroot}/%{_prefix}/include/pulsecore/modules/meego
install -d %{buildroot}/%{_prefix}/include/pulsecore/modules/sailfishos
install -m 644 src/common/include/sailfishos/*.h %{buildroot}/%{_prefix}/include/pulsecore/modules/sailfishos
install -d %{buildroot}/%{_libdir}/pkgconfig
install -m 644 src/common/*.pc %{buildroot}/%{_libdir}/pkgconfig


%files common
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulsemajorminor}/modules/libmeego-common.so
%license COPYING

%files music
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulsemajorminor}/modules/module-meego-music.so

%files record
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulsemajorminor}/modules/module-meego-record.so

%files voice
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulsemajorminor}/modules/module-meego-voice.so

%files mainvolume
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulsemajorminor}/modules/module-meego-mainvolume.so

%files parameters
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulsemajorminor}/modules/module-meego-parameters.so

%files sidetone
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulsemajorminor}/modules/module-meego-sidetone.so

%files test
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulsemajorminor}/modules/module-meego-test.so

%files stream-restore
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulsemajorminor}/modules/module-stream-restore-nemo.so

%files devel
%defattr(-,root,root,-)
%{_prefix}/include/pulsecore/modules/meego/*.h
%{_prefix}/include/pulsecore/modules/sailfishos/*.h
%{_libdir}/pkgconfig/*.pc
