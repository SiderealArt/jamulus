/******************************************************************************\
 * Copyright (c) 2004-2020
 *
 * Author(s):
 *  Volker Fischer
 *
 ******************************************************************************
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
\******************************************************************************/

#include "util.h"
#include "client.h"

/* Implementation *************************************************************/
// Input level meter implementation --------------------------------------------
void CStereoSignalLevelMeter::Update ( const CVector<short>& vecsAudio, const int iMonoBlockSizeSam, const bool bIsStereoIn )
{
    // Get maximum of current block
    //
    // Speed optimization:
    // - we only make use of the negative values and ignore the positive ones (since
    //   int16 has range {-32768, 32767}) -> we do not need to call the fabs() function
    // - we only evaluate every third sample
    //
    // With these speed optimizations we might loose some information in
    // special cases but for the average music signals the following code
    // should give good results.
    short sMinLOrMono = 0;
    short sMinR       = 0;

    if ( bIsStereoIn )
    {
        // stereo in
        for ( int i = 0; i < 2 * iMonoBlockSizeSam; i += 6 ) // 2 * 3 = 6 -> stereo
        {
            // left (or mono) and right channel
            sMinLOrMono = std::min ( sMinLOrMono, vecsAudio[i] );
            sMinR       = std::min ( sMinR, vecsAudio[i + 1] );
        }

        // in case of mono out use minimum of both channels
        if ( !bIsStereoOut )
        {
            sMinLOrMono = std::min ( sMinLOrMono, sMinR );
        }
    }
    else
    {
        // mono in
        for ( int i = 0; i < iMonoBlockSizeSam; i += 3 )
        {
            sMinLOrMono = std::min ( sMinLOrMono, vecsAudio[i] );
        }
    }

    // apply smoothing, if in stereo out mode, do this for two channels
    dCurLevelLOrMono = UpdateCurLevel ( dCurLevelLOrMono, -sMinLOrMono );

    if ( bIsStereoOut )
    {
        dCurLevelR = UpdateCurLevel ( dCurLevelR, -sMinR );
    }
}

double CStereoSignalLevelMeter::UpdateCurLevel ( double dCurLevel, const double dMax )
{
    // decrease max with time
    if ( dCurLevel >= METER_FLY_BACK )
    {
        dCurLevel *= dSmoothingFactor;
    }
    else
    {
        dCurLevel = 0;
    }

    // update current level -> only use maximum
    if ( dMax > dCurLevel )
    {
        return dMax;
    }
    else
    {
        return dCurLevel;
    }
}

double CStereoSignalLevelMeter::CalcLogResultForMeter ( const double& dLinearLevel )
{
    const double dNormLevel = dLinearLevel / _MAXSHORT;

    // logarithmic measure
    double dLevelForMeterdB = -100000.0; // large negative value

    if ( dNormLevel > 0 )
    {
        dLevelForMeterdB = 20.0 * log10 ( dNormLevel );
    }

    // map to signal level meter (linear transformation of the input
    // level range to the level meter range)
    dLevelForMeterdB -= LOW_BOUND_SIG_METER;
    dLevelForMeterdB *= NUM_STEPS_LED_BAR / ( UPPER_BOUND_SIG_METER - LOW_BOUND_SIG_METER );

    if ( dLevelForMeterdB < 0 )
    {
        dLevelForMeterdB = 0;
    }

    return dLevelForMeterdB;
}

// CRC -------------------------------------------------------------------------
void CCRC::Reset()
{
    // init state shift-register with ones. Set all registers to "1" with
    // bit-wise not operation
    iStateShiftReg = ~uint32_t ( 0 );
}

void CCRC::AddByte ( const uint8_t byNewInput )
{
    for ( int i = 0; i < 8; i++ )
    {
        // shift bits in shift-register for transition
        iStateShiftReg <<= 1;

        // take bit, which was shifted out of the register-size and place it
        // at the beginning (LSB)
        // (If condition is not satisfied, implicitly a "0" is added)
        if ( ( iStateShiftReg & iBitOutMask ) > 0 )
        {
            iStateShiftReg |= 1;
        }

        // add new data bit to the LSB
        if ( ( byNewInput & ( 1 << ( 8 - i - 1 ) ) ) > 0 )
        {
            iStateShiftReg ^= 1;
        }

        // add mask to shift-register if first bit is true
        if ( iStateShiftReg & 1 )
        {
            iStateShiftReg ^= iPoly;
        }
    }
}

uint32_t CCRC::GetCRC()
{
    // return inverted shift-register (1's complement)
    iStateShiftReg = ~iStateShiftReg;

    // remove bit which where shifted out of the shift-register frame
    return iStateShiftReg & ( iBitOutMask - 1 );
}

/******************************************************************************\
* Audio Reverberation                                                          *
\******************************************************************************/
/*
    The following code is based on "JCRev: John Chowning's reverberator class"
    by Perry R. Cook and Gary P. Scavone, 1995 - 2004
    which is in "The Synthesis ToolKit in C++ (STK)"
    http://ccrma.stanford.edu/software/stk

    Original description:
    This class is derived from the CLM JCRev function, which is based on the use
    of networks of simple allpass and comb delay filters. This class implements
    three series allpass units, followed by four parallel comb filters, and two
    decorrelation delay lines in parallel at the output.
*/
void CAudioReverb::Init ( const EAudChanConf eNAudioChannelConf, const int iNStereoBlockSizeSam, const int iSampleRate, const float fT60 )
{
    // store parameters
    eAudioChannelConf   = eNAudioChannelConf;
    iStereoBlockSizeSam = iNStereoBlockSizeSam;

    // delay lengths for 44100 Hz sample rate
    int         lengths[9] = { 1116, 1356, 1422, 1617, 225, 341, 441, 211, 179 };
    const float scaler     = static_cast<float> ( iSampleRate ) / 44100.0f;

    if ( scaler != 1.0f )
    {
        for ( int i = 0; i < 9; i++ )
        {
            int delay = static_cast<int> ( floorf ( scaler * lengths[i] ) );

            if ( ( delay & 1 ) == 0 )
            {
                delay++;
            }

            while ( !isPrime ( delay ) )
            {
                delay += 2;
            }

            lengths[i] = delay;
        }
    }

    for ( int i = 0; i < 3; i++ )
    {
        allpassDelays[i].Init ( lengths[i + 4] );
    }

    for ( int i = 0; i < 4; i++ )
    {
        combDelays[i].Init ( lengths[i] );
        combFilters[i].setPole ( 0.2f );
    }

    setT60 ( fT60, iSampleRate );
    outLeftDelay.Init ( lengths[7] );
    outRightDelay.Init ( lengths[8] );
    allpassCoefficient = 0.7f;
    Clear();
}

bool CAudioReverb::isPrime ( const int number )
{
    /*
        Returns true if argument value is prime. Taken from "class Effect" in
        "STK abstract effects parent class".
    */
    if ( number == 2 )
    {
        return true;
    }

    if ( number & 1 )
    {
        for ( int i = 3; i < static_cast<int> ( sqrtf ( static_cast<float> ( number ) ) ) + 1; i += 2 )
        {
            if ( ( number % i ) == 0 )
            {
                return false;
            }
        }

        return true; // prime
    }
    else
    {
        return false; // even
    }
}

void CAudioReverb::Clear()
{
    // reset and clear all internal state
    allpassDelays[0].Reset ( 0 );
    allpassDelays[1].Reset ( 0 );
    allpassDelays[2].Reset ( 0 );
    combDelays[0].Reset ( 0 );
    combDelays[1].Reset ( 0 );
    combDelays[2].Reset ( 0 );
    combDelays[3].Reset ( 0 );
    combFilters[0].Reset();
    combFilters[1].Reset();
    combFilters[2].Reset();
    combFilters[3].Reset();
    outRightDelay.Reset ( 0 );
    outLeftDelay.Reset ( 0 );
}

void CAudioReverb::setT60 ( const float fT60, const int iSampleRate )
{
    // set the reverberation T60 decay time
    for ( int i = 0; i < 4; i++ )
    {
        combCoefficient[i] = powf ( 10.0f, static_cast<float> ( -3.0f * combDelays[i].Size() / ( fT60 * iSampleRate ) ) );
    }
}

void CAudioReverb::COnePole::setPole ( const float fPole )
{
    // calculate IIR filter coefficients based on the pole value
    fA = -fPole;
    fB = 1.0f - fPole;
}

float CAudioReverb::COnePole::Calc ( const float fIn )
{
    // calculate IIR filter
    fLastSample = fB * fIn - fA * fLastSample;

    return fLastSample;
}

void CAudioReverb::Process ( CVector<int16_t>& vecsStereoInOut, const bool bReverbOnLeftChan, const float fAttenuation )
{
    float fMixedInput, temp, temp0, temp1, temp2;

    for ( int i = 0; i < iStereoBlockSizeSam; i += 2 )
    {
        // we sum up the stereo input channels (in case mono input is used, a zero
        // shall be input for the right channel)
        if ( eAudioChannelConf == CC_STEREO )
        {
            fMixedInput = 0.5f * ( vecsStereoInOut[i] + vecsStereoInOut[i + 1] );
        }
        else
        {
            if ( bReverbOnLeftChan )
            {
                fMixedInput = vecsStereoInOut[i];
            }
            else
            {
                fMixedInput = vecsStereoInOut[i + 1];
            }
        }

        temp  = allpassDelays[0].Get();
        temp0 = allpassCoefficient * temp;
        temp0 += fMixedInput;
        allpassDelays[0].Add ( temp0 );
        temp0 = -( allpassCoefficient * temp0 ) + temp;

        temp  = allpassDelays[1].Get();
        temp1 = allpassCoefficient * temp;
        temp1 += temp0;
        allpassDelays[1].Add ( temp1 );
        temp1 = -( allpassCoefficient * temp1 ) + temp;

        temp  = allpassDelays[2].Get();
        temp2 = allpassCoefficient * temp;
        temp2 += temp1;
        allpassDelays[2].Add ( temp2 );
        temp2 = -( allpassCoefficient * temp2 ) + temp;

        const float temp3 = temp2 + combFilters[0].Calc ( combCoefficient[0] * combDelays[0].Get() );
        const float temp4 = temp2 + combFilters[1].Calc ( combCoefficient[1] * combDelays[1].Get() );
        const float temp5 = temp2 + combFilters[2].Calc ( combCoefficient[2] * combDelays[2].Get() );
        const float temp6 = temp2 + combFilters[3].Calc ( combCoefficient[3] * combDelays[3].Get() );

        combDelays[0].Add ( temp3 );
        combDelays[1].Add ( temp4 );
        combDelays[2].Add ( temp5 );
        combDelays[3].Add ( temp6 );

        const float filtout = temp3 + temp4 + temp5 + temp6;

        outLeftDelay.Add ( filtout );
        outRightDelay.Add ( filtout );

        // inplace apply the attenuated reverb signal (for stereo always apply
        // reverberation effect on both channels)
        if ( ( eAudioChannelConf == CC_STEREO ) || bReverbOnLeftChan )
        {
            vecsStereoInOut[i] = Float2Short ( ( 1.0f - fAttenuation ) * vecsStereoInOut[i] + 0.5f * fAttenuation * outLeftDelay.Get() );
        }

        if ( ( eAudioChannelConf == CC_STEREO ) || !bReverbOnLeftChan )
        {
            vecsStereoInOut[i + 1] = Float2Short ( ( 1.0f - fAttenuation ) * vecsStereoInOut[i + 1] + 0.5f * fAttenuation * outRightDelay.Get() );
        }
    }
}

/******************************************************************************\
* GUI Utilities                                                                *
\******************************************************************************/
// About dialog ----------------------------------------------------------------
#ifndef HEADLESS
CAboutDlg::CAboutDlg ( QWidget* parent ) : CBaseDlg ( parent )
{
    setupUi ( this );

    // general description of software
    txvAbout->setText ( "<p>" +
                        tr ( "This app enables musicians to perform real-time jam sessions "
                             "over the internet." ) +
                        "<br>" +
                        tr ( "There is a server which collects "
                             " the audio data from each client, mixes the audio data and sends the mix "
                             " back to each client." ) +
                        "</p>"
                        "<p><font face=\"courier\">" // GPL header text
                        "This program is free software; you can redistribute it and/or modify "
                        "it under the terms of the GNU General Public License as published by "
                        "the Free Software Foundation; either version 2 of the License, or "
                        "(at your option) any later version.<br>This program is distributed in "
                        "the hope that it will be useful, but WITHOUT ANY WARRANTY; without "
                        "even the implied warranty of MERCHANTABILITY or FITNESS FOR A "
                        "PARTICULAR PURPOSE. See the GNU General Public License for more "
                        "details.<br>You should have received a copy of the GNU General Public "
                        "License along with his program; if not, write to the Free Software "
                        "Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 "
                        "USA"
                        "</font></p>" );

    // libraries used by this compilation
    txvLibraries->setText (
        tr ( "This app uses the following libraries, resources or code snippets:" ) + "<br><p>" + tr ( "Qt cross-platform application framework" ) +
        ", <i><a href=\"http://www.qt.io\">http://www.qt.io</a></i></p>"
        "<p>Opus Interactive Audio Codec"
        ", <i><a href=\"http://www.opus-codec.org\">http://www.opus-codec.org</a></i></p>"
        "<p>" +
        tr ( "Audio reverberation code by Perry R. Cook and Gary P. Scavone" ) +
        ", 1995 - 2004, <i><a href=\"http://ccrma.stanford.edu/software/stk\">"
        "The Synthesis ToolKit in C++ (STK)</a></i></p>"
        "<p>" +
        tr ( "Some pixmaps are from the" ) +
        " Open Clip Art Library (OCAL), "
        "<i><a href=\"http://openclipart.org\">http://openclipart.org</a></i></p>"
        "<p>" +
        tr ( "Country flag icons by Mark James" ) + ", <i><a href=\"http://www.famfamfam.com\">http://www.famfamfam.com</a></i></p>" );

    // contributors list
    txvContributors->setText ( "<p>Volker Fischer (<a href=\"https://github.com/corrados\">corrados</a>)</p>"
                               "<p>Peter L. Jones (<a href=\"https://github.com/pljones\">pljones</a>)</p>"
                               "<p>Jonathan Baker-Bates (<a href=\"https://github.com/gilgongo\">gilgongo</a>)</p>"
                               "<p>ann0see (<a href=\"https://github.com/ann0see\">ann0see</a>)</p>"
                               "<p>Daniele Masato (<a href=\"https://github.com/doloopuntil\">doloopuntil</a>)</p>"
                               "<p>Martin Schilde (<a href=\"https://github.com/geheimerEichkater\">geheimerEichkater</a>)</p>"
                               "<p>Simon Tomlinson (<a href=\"https://github.com/sthenos\">sthenos</a>)</p>"
                               "<p>Marc jr. Landolt (<a href=\"https://github.com/braindef\">braindef</a>)</p>"
                               "<p>Olivier Humbert (<a href=\"https://github.com/trebmuh\">trebmuh</a>)</p>"
                               "<p>Tarmo Johannes (<a href=\"https://github.com/tarmoj\">tarmoj</a>)</p>"
                               "<p>mirabilos (<a href=\"https://github.com/mirabilos\">mirabilos</a>)</p>"
                               "<p>Hector Martin (<a href=\"https://github.com/marcan\">marcan</a>)</p>"
                               "<p>newlaurent62 (<a href=\"https://github.com/newlaurent62\">newlaurent62</a>)</p>"
                               "<p>AronVietti (<a href=\"https://github.com/AronVietti\">AronVietti</a>)</p>"
                               "<p>Emlyn Bolton (<a href=\"https://github.com/emlynmac\">emlynmac</a>)</p>"
                               "<p>Jos van den Oever (<a href=\"https://github.com/vandenoever\">vandenoever</a>)</p>"
                               "<p>Tormod Volden (<a href=\"https://github.com/tormodvolden\">tormodvolden</a>)</p>"
                               "<p>Alberstein8 (<a href=\"https://github.com/Alberstein8\">Alberstein8</a>)</p>"
                               "<p>Gauthier Fleutot Östervall (<a href=\"https://github.com/fleutot\">fleutot</a>)</p>"
                               "<p>Tony Mountifield (<a href=\"https://github.com/softins\">softins</a>)</p>"
                               "<p>HPS (<a href=\"https://github.com/hselasky\">hselasky</a>)</p>"
                               "<p>Stanislas Michalak (<a href=\"https://github.com/stanislas-m\">stanislas-m</a>)</p>"
                               "<p>JP Cimalando (<a href=\"https://github.com/jpcima\">jpcima</a>)</p>"
                               "<p>Adam Sampson (<a href=\"https://github.com/atsampson\">atsampson</a>)</p>"
                               "<p>Jakob Jarmar (<a href=\"https://github.com/jarmar\">jarmar</a>)</p>"
                               "<p>Stefan Weil (<a href=\"https://github.com/stweil\">stweil</a>)</p>"
                               "<p>Nils Brederlow (<a href=\"https://github.com/dingodoppelt\">dingodoppelt</a>)</p>"
                               "<p>Sebastian Krzyszkowiak (<a href=\"https://github.com/dos1\">dos1</a>)</p>"
                               "<p>Bryan Flamig (<a href=\"https://github.com/bflamig\">bflamig</a>)</p>"
                               "<p>Kris Raney (<a href=\"https://github.com/kraney\">kraney</a>)</p>"
                               "<p>dszgit (<a href=\"https://github.com/dszgit\">dszgit</a>)</p>"
                               "<p>nefarius2001 (<a href=\"https://github.com/nefarius2001\">nefarius2001</a>)</p>"
                               "<p>jc-Rosichini (<a href=\"https://github.com/jc-Rosichini\">jc-Rosichini</a>)</p>"
                               "<p>Julian Santander (<a href=\"https://github.com/j-santander\">j-santander</a>)</p>"
                               "<p>chigkim (<a href=\"https://github.com/chigkim\">chigkim</a>)</p>"
                               "<p>Bodo (<a href=\"https://github.com/bomm\">bomm</a>)</p>"
                               "<p>Christian Hoffmann (<a href=\"https://github.com/hoffie\">hoffie</a>)</p>"
                               "<p>jp8 (<a href=\"https://github.com/jp8\">jp8</a>)</p>"
                               "<p>James (<a href=\"https://github.com/jdrage\">jdrage</a>)</p>"
                               "<p>ranfdev (<a href=\"https://github.com/ranfdev\">ranfdev</a>)</p>"
                               "<p>bspeer (<a href=\"https://github.com/bspeer\">bspeer</a>)</p>"
                               "<p>Martin Passing (<a href=\"https://github.com/passing\">passing</a>)</p>"
                               "<p>DonC (<a href=\"https://github.com/dcorson-ticino-com\">dcorson-ticino-com</a>)</p>"
                               "<p>David Kastrup (<a href=\"https://github.com/dakhubgit\">dakhubgit</a>)</p>"
                               "<p>Jordan Lum (<a href=\"https://github.com/mulyaj\">mulyaj</a>)</p>"
                               "<p>Noam Postavsky (<a href=\"https://github.com/npostavs\">npostavs</a>)</p>"
                               "<p>David Savinkoff (<a href=\"https://github.com/DavidSavinkoff\">DavidSavinkoff</a>)</p>"
                               "<p>Johannes Brauers (<a href=\"https://github.com/JohannesBrx\">JohannesBrx</a>)</p>"
                               "<p>Henk De Groot (<a href=\"https://github.com/henkdegroot\">henkdegroot</a>)</p>"
                               "<p>Ferenc Wágner (<a href=\"https://github.com/wferi\">wferi</a>)</p>"
                               "<p>Martin Kaistra (<a href=\"https://github.com/djfun\">djfun</a>)</p>"
                               "<p>Burkhard Volkemer (<a href=\"https://github.com/buv\">buv</a>)</p>"
                               "<p>Magnus Groß (<a href=\"https://github.com/vimpostor\">vimpostor</a>)</p>"
                               "<p>Julien Taverna (<a href=\"https://github.com/jujudusud\">jujudusud</a>)</p>"
                               "<p>Detlef Hennings (<a href=\"https://github.com/DetlefHennings\">DetlefHennings</a>)</p>"
                               "<p>drummer1154 (<a href=\"https://github.com/drummer1154\">drummer1154</a>)</p>"
                               "<p>helgeerbe (<a href=\"https://github.com/helgeerbe\">helgeerbe</a>)</p>"
                               "<p>Hk1020 (<a href=\"https://github.com/Hk1020\">Hk1020</a>)</p>"
                               "<p>Jeroen van Veldhuizen (<a href=\"https://github.com/jeroenvv\">jeroenvv</a>)</p>"
                               "<p>Reinhard (<a href=\"https://github.com/reinhardwh\">reinhardwh</a>)</p>"
                               "<p>Stefan Menzel (<a href=\"https://github.com/menzels\">menzels</a>)</p>"
                               "<p>Dau Huy Ngoc (<a href=\"https://github.com/ngocdh\">ngocdh</a>)</p>"
                               "<p>Jiri Popek (<a href=\"https://github.com/jardous\">jardous</a>)</p>"
                               "<p>Gary Wang (<a href=\"https://github.com/BLumia\">BLumia</a>)</p>"
                               "<br>" +
                               tr ( "For details on the contributions check out the " ) +
                               "<a href=\"https://github.com/jamulussoftware/jamulus/graphs/contributors\">" + tr ( "Github Contributors list" ) +
                               "</a>." );

    // translators
    txvTranslation->setText ( "<p><b>" + tr ( "Spanish" ) +
                              "</b></p>"
                              "<p>Daryl Hanlon (<a href=\"https://github.com/ignotus666\">ignotus666</a>)</p>"
                              "<p><b>" +
                              tr ( "French" ) +
                              "</b></p>"
                              "<p>Olivier Humbert (<a href=\"https://github.com/trebmuh\">trebmuh</a>)</p>"
                              "<p>Julien Taverna (<a href=\"https://github.com/jujudusud\">jujudusud</a>)</p>"
                              "<p><b>" +
                              tr ( "Portuguese" ) +
                              "</b></p>"
                              "<p>Miguel de Matos (<a href=\"https://github.com/Snayler\">Snayler</a>)</p>"
                              "<p>Melcon Moraes (<a href=\"https://github.com/melcon\">melcon</a>)</p>"
                              "<p><b>" +
                              tr ( "Dutch" ) +
                              "</b></p>"
                              "<p>Jeroen Geertzen (<a href=\"https://github.com/jerogee\">jerogee</a>)</p>"
                              "<p>Henk De Groot (<a href=\"https://github.com/henkdegroot\">henkdegroot</a>)</p>"
                              "<p><b>" +
                              tr ( "Italian" ) +
                              "</b></p>"
                              "<p>Giuseppe Sapienza (<a href=\"https://github.com/dzpex\">dzpex</a>)</p>"
                              "<p><b>" +
                              tr ( "German" ) +
                              "</b></p>"
                              "<p>Volker Fischer (<a href=\"https://github.com/corrados\">corrados</a>)</p>"
                              "<p>Roland Moschel (<a href=\"https://github.com/rolamos\">rolamos</a>)</p>"
                              "<p><b>" +
                              tr ( "Polish" ) +
                              "</b></p>"
                              "<p>Martyna Danysz (<a href=\"https://github.com/Martyna27\">Martyna27</a>)</p>"
                              "<p>Tomasz Bojczuk (<a href=\"https://github.com/SeeLook\">SeeLook</a>)</p>"
                              "<p><b>" +
                              tr ( "Swedish" ) +
                              "</b></p>"
                              "<p>Daniel (<a href=\"https://github.com/genesisproject2020\">genesisproject2020</a>)</p>"
                              "<p><b>" +
                              tr ( "Slovak" ) +
                              "</b></p>"
                              "<p>Jose Riha (<a href=\"https://github.com/jose1711\">jose1711</a>)</p>" +
                              "<p><b>" + tr ( "Simplified Chinese" ) +
                              "</b></p>"
                              "<p>Gary Wang (<a href=\"https://github.com/BLumia\">BLumia</a>)</p>" );

    // set version number in about dialog
    lblVersion->setText ( GetVersionAndNameStr() );

    // set window title
    setWindowTitle ( tr ( "About " ) + APP_NAME );
}

// Licence dialog --------------------------------------------------------------
CLicenceDlg::CLicenceDlg ( QWidget* parent ) : CBaseDlg ( parent )
{
    /*
        The licence dialog is structured as follows:
        - text box with the licence text on the top
        - check box: I &agree to the above licence terms
        - Accept button (disabled if check box not checked)
        - Decline button
    */
    setWindowIcon ( QIcon ( QString::fromUtf8 ( ":/png/main/res/fronticon.png" ) ) );

    QVBoxLayout* pLayout    = new QVBoxLayout ( this );
    QHBoxLayout* pSubLayout = new QHBoxLayout;
    QLabel*      lblLicence =
        new QLabel ( tr ( "This server requires you accept conditions before you can join. Please read these in the chat window." ), this );
    QCheckBox* chbAgree     = new QCheckBox ( tr ( "I have read the conditions and &agree." ), this );
    butAccept               = new QPushButton ( tr ( "Accept" ), this );
    QPushButton* butDecline = new QPushButton ( tr ( "Decline" ), this );

    pSubLayout->addStretch();
    pSubLayout->addWidget ( chbAgree );
    pSubLayout->addWidget ( butAccept );
    pSubLayout->addWidget ( butDecline );
    pLayout->addWidget ( lblLicence );
    pLayout->addLayout ( pSubLayout );

    // set some properties
    butAccept->setEnabled ( false );
    butAccept->setDefault ( true );

    QObject::connect ( chbAgree, &QCheckBox::stateChanged, this, &CLicenceDlg::OnAgreeStateChanged );

    QObject::connect ( butAccept, &QPushButton::clicked, this, &CLicenceDlg::accept );

    QObject::connect ( butDecline, &QPushButton::clicked, this, &CLicenceDlg::reject );
}

// Help menu -------------------------------------------------------------------
CHelpMenu::CHelpMenu ( const bool bIsClient, QWidget* parent ) : QMenu ( tr ( "&Help" ), parent )
{
    QAction* pAction;

    // standard help menu consists of about and what's this help
    if ( bIsClient )
    {
        addAction ( tr ( "Getting &Started..." ), this, SLOT ( OnHelpClientGetStarted() ) );
        addAction ( tr ( "Software &Manual..." ), this, SLOT ( OnHelpSoftwareMan() ) );
    }
    else
    {
        addAction ( tr ( "Getting &Started..." ), this, SLOT ( OnHelpServerGetStarted() ) );
    }
    addSeparator();
    addAction ( tr ( "What's &This" ), this, SLOT ( OnHelpWhatsThis() ), QKeySequence ( Qt::SHIFT + Qt::Key_F1 ) );
    addSeparator();
    pAction = addAction ( tr ( "&About Jamulus..." ), this, SLOT ( OnHelpAbout() ) );
    pAction->setMenuRole ( QAction::AboutRole ); // required for Mac
    pAction = addAction ( tr ( "About &Qt..." ), this, SLOT ( OnHelpAboutQt() ) );
    pAction->setMenuRole ( QAction::AboutQtRole ); // required for Mac
}

// Language combo box ----------------------------------------------------------
CLanguageComboBox::CLanguageComboBox ( QWidget* parent ) : QComboBox ( parent ), iIdxSelectedLanguage ( INVALID_INDEX )
{
    QObject::connect ( this, static_cast<void ( QComboBox::* ) ( int )> ( &QComboBox::activated ), this, &CLanguageComboBox::OnLanguageActivated );
}

void CLanguageComboBox::Init ( QString& strSelLanguage )
{
    // load available translations
    const QMap<QString, QString>   TranslMap = CLocale::GetAvailableTranslations();
    QMapIterator<QString, QString> MapIter ( TranslMap );

    // add translations to the combobox list
    clear();
    int iCnt                  = 0;
    int iIdxOfEnglishLanguage = 0;
    iIdxSelectedLanguage      = INVALID_INDEX;

    while ( MapIter.hasNext() )
    {
        MapIter.next();
        addItem ( QLocale ( MapIter.key() ).nativeLanguageName() + " (" + MapIter.key() + ")", MapIter.key() );

        // store the combo box index of the default english language
        if ( MapIter.key().compare ( "en" ) == 0 )
        {
            iIdxOfEnglishLanguage = iCnt;
        }

        // if the selected language is found, store the combo box index
        if ( MapIter.key().compare ( strSelLanguage ) == 0 )
        {
            iIdxSelectedLanguage = iCnt;
        }

        iCnt++;
    }

    // if the selected language was not found, use the english language
    if ( iIdxSelectedLanguage == INVALID_INDEX )
    {
        strSelLanguage       = "en";
        iIdxSelectedLanguage = iIdxOfEnglishLanguage;
    }

    setCurrentIndex ( iIdxSelectedLanguage );
}

void CLanguageComboBox::OnLanguageActivated ( int iLanguageIdx )
{
    // only update if the language selection is different from the current selected language
    if ( iIdxSelectedLanguage != iLanguageIdx )
    {
        QMessageBox::information ( this, tr ( "Restart Required" ), tr ( "Please restart the application for the language change to take effect." ) );

        emit LanguageChanged ( itemData ( iLanguageIdx ).toString() );
    }
}

static inline QString TruncateString ( QString str, int position )
{
    QTextBoundaryFinder tbfString ( QTextBoundaryFinder::Grapheme, str );

    tbfString.setPosition ( position );
    if ( !tbfString.isAtBoundary() )
    {
        tbfString.toPreviousBoundary();
        position = tbfString.position();
    }
    return str.left ( position );
}
#endif

/******************************************************************************\
* Other Classes                                                                *
\******************************************************************************/
// Network utility functions ---------------------------------------------------
bool NetworkUtil::ParseNetworkAddress ( QString strAddress, CHostAddress& HostAddress, bool bEnableIPv6 )
{
    QHostAddress InetAddr;
    unsigned int iNetPort = DEFAULT_PORT_NUMBER;

    // qInfo() << qUtf8Printable ( QString ( "Parsing network address %1" ).arg ( strAddress ) );

    // init requested host address with invalid address first
    HostAddress = CHostAddress();

    // Allow the following address formats:
    // [addr4or6]
    // [addr4or6]:port
    // addr4
    // addr4:port
    // hostname
    // hostname:port
    // (where addr4or6 is a literal IPv4 or IPv6 address, and addr4 is a literal IPv4 address

    bool    bLiteralAddr = false;
    QRegExp rx1 ( "^\\[([^]]*)\\](?::(\\d+))?$" ); // [addr4or6] or [addr4or6]:port
    QRegExp rx2 ( "^([^:]*)(?::(\\d+))?$" );       // addr4 or addr4:port or host or host:port

    QString strPort;

    // parse input address with rx1 and rx2 in turn, capturing address/host and port
    if ( rx1.indexIn ( strAddress ) == 0 )
    {
        // literal address within []
        strAddress   = rx1.cap ( 1 );
        strPort      = rx1.cap ( 2 );
        bLiteralAddr = true; // don't allow hostname within []
    }
    else if ( rx2.indexIn ( strAddress ) == 0 )
    {
        // hostname or IPv4 address
        strAddress = rx2.cap ( 1 );
        strPort    = rx2.cap ( 2 );
    }
    else
    {
        // invalid format
        // qInfo() << qUtf8Printable ( QString ( "Invalid address format" ) );
        return false;
    }

    if ( !strPort.isEmpty() )
    {
        // a port number was given: extract port number
        iNetPort = strPort.toInt();

        if ( iNetPort >= 65536 )
        {
            // invalid port number
            // qInfo() << qUtf8Printable ( QString ( "Invalid port number specified" ) );
            return false;
        }
    }

    // first try if this is an IP number an can directly applied to QHostAddress
    if ( InetAddr.setAddress ( strAddress ) )
    {
        if ( !bEnableIPv6 && InetAddr.protocol() == QAbstractSocket::IPv6Protocol )
        {
            // do not allow IPv6 addresses if not enabled
            // qInfo() << qUtf8Printable ( QString ( "IPv6 addresses disabled" ) );
            return false;
        }
    }
    else
    {
        // it was no valid IP address. If literal required, return as invalid
        if ( bLiteralAddr )
        {
            // qInfo() << qUtf8Printable ( QString ( "Invalid literal IP address" ) );
            return false; // invalid address
        }

        // try to get host by name, assuming
        // that the string contains a valid host name string
        const QHostInfo HostInfo = QHostInfo::fromName ( strAddress );

        if ( HostInfo.error() != QHostInfo::NoError )
        {
            // qInfo() << qUtf8Printable ( QString ( "Invalid hostname" ) );
            return false; // invalid address
        }

        bool bFoundAddr = false;

        foreach ( const QHostAddress HostAddr, HostInfo.addresses() )
        {
            // qInfo() << qUtf8Printable ( QString ( "Resolved network address to %1 for proto %2" ) .arg ( HostAddr.toString() ) .arg (
            // HostAddr.protocol() ) );
            if ( HostAddr.protocol() == QAbstractSocket::IPv4Protocol || ( bEnableIPv6 && HostAddr.protocol() == QAbstractSocket::IPv6Protocol ) )
            {
                InetAddr   = HostAddr;
                bFoundAddr = true;
                break;
            }
        }

        if ( !bFoundAddr )
        {
            // no valid address found
            // qInfo() << qUtf8Printable ( QString ( "No IP address found for hostname" ) );
            return false;
        }
    }

    // qInfo() << qUtf8Printable ( QString ( "Parsed network address %1" ).arg ( InetAddr.toString() ) );

    HostAddress = CHostAddress ( InetAddr, iNetPort );

    return true;
}

CHostAddress NetworkUtil::GetLocalAddress()
{
    QUdpSocket socket;
    // As we are using UDP, the connectToHost() does not generate any traffic at all.
    // We just require a socket which is pointed towards the Internet in
    // order to find out the IP of our own external interface:
    socket.connectToHost ( WELL_KNOWN_HOST, WELL_KNOWN_PORT );

    if ( socket.waitForConnected ( IP_LOOKUP_TIMEOUT ) )
    {
        return CHostAddress ( socket.localAddress(), 0 );
    }
    else
    {
        qWarning() << "could not determine local IPv4 address:" << socket.errorString() << "- using localhost";

        return CHostAddress ( QHostAddress::LocalHost, 0 );
    }
}

CHostAddress NetworkUtil::GetLocalAddress6()
{
    QUdpSocket socket;
    // As we are using UDP, the connectToHost() does not generate any traffic at all.
    // We just require a socket which is pointed towards the Internet in
    // order to find out the IP of our own external interface:
    socket.connectToHost ( WELL_KNOWN_HOST6, WELL_KNOWN_PORT );

    if ( socket.waitForConnected ( IP_LOOKUP_TIMEOUT ) )
    {
        return CHostAddress ( socket.localAddress(), 0 );
    }
    else
    {
        qWarning() << "could not determine local IPv6 address:" << socket.errorString() << "- using localhost";

        return CHostAddress ( QHostAddress::LocalHostIPv6, 0 );
    }
}

QString NetworkUtil::GetDirectoryAddress ( const EDirectoryType eDirectoryType, const QString& strDirectoryAddress )
{
    switch ( eDirectoryType )
    {
    case AT_CUSTOM:
        return strDirectoryAddress;
    case AT_ANY_GENRE2:
        return CENTSERV_ANY_GENRE2;
    case AT_ANY_GENRE3:
        return CENTSERV_ANY_GENRE3;
    case AT_GENRE_ROCK:
        return CENTSERV_GENRE_ROCK;
    case AT_GENRE_JAZZ:
        return CENTSERV_GENRE_JAZZ;
    case AT_GENRE_CLASSICAL_FOLK:
        return CENTSERV_GENRE_CLASSICAL_FOLK;
    case AT_GENRE_CHORAL:
        return CENTSERV_GENRE_CHORAL;
    default:
        return DEFAULT_SERVER_ADDRESS; // AT_DEFAULT
    }
}

QString NetworkUtil::FixAddress ( const QString& strAddress )
{
    // remove all spaces from the address string
    return strAddress.simplified().replace ( " ", "" );
}

// Return whether the given HostAdress is within a private IP range
// as per RFC 1918 & RFC 5735.
bool NetworkUtil::IsPrivateNetworkIP ( const QHostAddress& qhAddr )
{
    // https://www.rfc-editor.org/rfc/rfc1918
    // https://www.rfc-editor.org/rfc/rfc5735
    static QList<QPair<QHostAddress, int>> addresses = {
        QPair<QHostAddress, int> ( QHostAddress ( "10.0.0.0" ), 8 ),
        QPair<QHostAddress, int> ( QHostAddress ( "127.0.0.0" ), 8 ),
        QPair<QHostAddress, int> ( QHostAddress ( "172.16.0.0" ), 12 ),
        QPair<QHostAddress, int> ( QHostAddress ( "192.168.0.0" ), 16 ),
    };

    foreach ( auto item, addresses )
    {
        if ( qhAddr.isInSubnet ( item ) )
        {
            return true;
        }
    }
    return false;
}

// CHostAddress methods
// Compare() - compare two CHostAddress objects, and return an ordering between them:
// 0 - they are equal
// <0 - this comes before other
// >0 - this comes after other
// The order is not important, so long as it is consistent, for use in a binary search.

int CHostAddress::Compare ( const CHostAddress& other ) const
{
    // compare port first, as it is cheap, and clients will often use random ports

    if ( iPort != other.iPort )
    {
        return (int) iPort - (int) other.iPort;
    }

    // compare protocols before addresses

    QAbstractSocket::NetworkLayerProtocol thisProto  = InetAddr.protocol();
    QAbstractSocket::NetworkLayerProtocol otherProto = other.InetAddr.protocol();

    if ( thisProto != otherProto )
    {
        return (int) thisProto - (int) otherProto;
    }

    // now we know both addresses are the same protocol

    if ( thisProto == QAbstractSocket::IPv6Protocol )
    {
        // compare IPv6 addresses
        Q_IPV6ADDR thisAddr  = InetAddr.toIPv6Address();
        Q_IPV6ADDR otherAddr = other.InetAddr.toIPv6Address();

        return memcmp ( &thisAddr, &otherAddr, sizeof ( Q_IPV6ADDR ) );
    }

    // compare IPv4 addresses
    quint32 thisAddr  = InetAddr.toIPv4Address();
    quint32 otherAddr = other.InetAddr.toIPv4Address();

    return thisAddr < otherAddr ? -1 : thisAddr > otherAddr ? 1 : 0;
}

// Instrument picture data base ------------------------------------------------
CVector<CInstPictures::CInstPictProps>& CInstPictures::GetTable ( const bool bReGenerateTable )
{
    // make sure we generate the table only once
    static bool TableIsInitialized = false;

    static CVector<CInstPictProps> vecDataBase;

    if ( !TableIsInitialized || bReGenerateTable )
    {
        // instrument picture data base initialization
        // NOTE: Do not change the order of any instrument in the future!
        // NOTE: The very first entry is the "not used" element per definition.
        vecDataBase.Init ( 0 ); // first clear all existing data since we create the list be adding entries
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "None" ),
                                           ":/png/instr/res/instruments/none.png",
                                           IC_OTHER_INSTRUMENT ) ); // special first element
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Drum Set" ),
                                           ":/png/instr/res/instruments/drumset.png",
                                           IC_PERCUSSION_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Djembe" ),
                                           ":/png/instr/res/instruments/djembe.png",
                                           IC_PERCUSSION_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Electric Guitar" ),
                                           ":/png/instr/res/instruments/eguitar.png",
                                           IC_PLUCKING_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Acoustic Guitar" ),
                                           ":/png/instr/res/instruments/aguitar.png",
                                           IC_PLUCKING_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Bass Guitar" ),
                                           ":/png/instr/res/instruments/bassguitar.png",
                                           IC_PLUCKING_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Keyboard" ),
                                           ":/png/instr/res/instruments/keyboard.png",
                                           IC_KEYBOARD_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Synthesizer" ),
                                           ":/png/instr/res/instruments/synthesizer.png",
                                           IC_KEYBOARD_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Grand Piano" ),
                                           ":/png/instr/res/instruments/grandpiano.png",
                                           IC_KEYBOARD_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Accordion" ),
                                           ":/png/instr/res/instruments/accordeon.png",
                                           IC_KEYBOARD_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Vocal" ),
                                           ":/png/instr/res/instruments/vocal.png",
                                           IC_OTHER_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Microphone" ),
                                           ":/png/instr/res/instruments/microphone.png",
                                           IC_OTHER_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Harmonica" ),
                                           ":/png/instr/res/instruments/harmonica.png",
                                           IC_WIND_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Trumpet" ),
                                           ":/png/instr/res/instruments/trumpet.png",
                                           IC_WIND_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Trombone" ),
                                           ":/png/instr/res/instruments/trombone.png",
                                           IC_WIND_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "French Horn" ),
                                           ":/png/instr/res/instruments/frenchhorn.png",
                                           IC_WIND_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Tuba" ),
                                           ":/png/instr/res/instruments/tuba.png",
                                           IC_WIND_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Saxophone" ),
                                           ":/png/instr/res/instruments/saxophone.png",
                                           IC_WIND_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Clarinet" ),
                                           ":/png/instr/res/instruments/clarinet.png",
                                           IC_WIND_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Flute" ),
                                           ":/png/instr/res/instruments/flute.png",
                                           IC_WIND_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Violin" ),
                                           ":/png/instr/res/instruments/violin.png",
                                           IC_STRING_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Cello" ),
                                           ":/png/instr/res/instruments/cello.png",
                                           IC_STRING_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Double Bass" ),
                                           ":/png/instr/res/instruments/doublebass.png",
                                           IC_STRING_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Recorder" ),
                                           ":/png/instr/res/instruments/recorder.png",
                                           IC_OTHER_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Streamer" ),
                                           ":/png/instr/res/instruments/streamer.png",
                                           IC_OTHER_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Listener" ),
                                           ":/png/instr/res/instruments/listener.png",
                                           IC_OTHER_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Guitar+Vocal" ),
                                           ":/png/instr/res/instruments/guitarvocal.png",
                                           IC_MULTIPLE_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Keyboard+Vocal" ),
                                           ":/png/instr/res/instruments/keyboardvocal.png",
                                           IC_MULTIPLE_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Bodhran" ),
                                           ":/png/instr/res/instruments/bodhran.png",
                                           IC_PERCUSSION_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Bassoon" ),
                                           ":/png/instr/res/instruments/bassoon.png",
                                           IC_WIND_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Oboe" ),
                                           ":/png/instr/res/instruments/oboe.png",
                                           IC_WIND_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Harp" ),
                                           ":/png/instr/res/instruments/harp.png",
                                           IC_STRING_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Viola" ),
                                           ":/png/instr/res/instruments/viola.png",
                                           IC_STRING_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Congas" ),
                                           ":/png/instr/res/instruments/congas.png",
                                           IC_PERCUSSION_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Bongo" ),
                                           ":/png/instr/res/instruments/bongo.png",
                                           IC_PERCUSSION_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Vocal Bass" ),
                                           ":/png/instr/res/instruments/vocalbass.png",
                                           IC_OTHER_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Vocal Tenor" ),
                                           ":/png/instr/res/instruments/vocaltenor.png",
                                           IC_OTHER_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Vocal Alto" ),
                                           ":/png/instr/res/instruments/vocalalto.png",
                                           IC_OTHER_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Vocal Soprano" ),
                                           ":/png/instr/res/instruments/vocalsoprano.png",
                                           IC_OTHER_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Banjo" ),
                                           ":/png/instr/res/instruments/banjo.png",
                                           IC_PLUCKING_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Mandolin" ),
                                           ":/png/instr/res/instruments/mandolin.png",
                                           IC_PLUCKING_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Ukulele" ),
                                           ":/png/instr/res/instruments/ukulele.png",
                                           IC_PLUCKING_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Bass Ukulele" ),
                                           ":/png/instr/res/instruments/bassukulele.png",
                                           IC_PLUCKING_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Vocal Baritone" ),
                                           ":/png/instr/res/instruments/vocalbaritone.png",
                                           IC_OTHER_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Vocal Lead" ),
                                           ":/png/instr/res/instruments/vocallead.png",
                                           IC_OTHER_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Mountain Dulcimer" ),
                                           ":/png/instr/res/instruments/mountaindulcimer.png",
                                           IC_STRING_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Scratching" ),
                                           ":/png/instr/res/instruments/scratching.png",
                                           IC_OTHER_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Rapping" ),
                                           ":/png/instr/res/instruments/rapping.png",
                                           IC_OTHER_INSTRUMENT ) );
        vecDataBase.Add ( CInstPictProps ( QCoreApplication::translate ( "CClientSettingsDlg", "Vibraphone" ),
                                           ":/png/instr/res/instruments/vibraphone.png",
                                           IC_PERCUSSION_INSTRUMENT ) );

        // now the table is initialized
        TableIsInitialized = true;
    }

    return vecDataBase;
}

bool CInstPictures::IsInstIndexInRange ( const int iIdx )
{
    // check if index is in valid range
    return ( iIdx >= 0 ) && ( iIdx < GetTable().Size() );
}

QString CInstPictures::GetResourceReference ( const int iInstrument )
{
    // range check
    if ( IsInstIndexInRange ( iInstrument ) )
    {
        // return the string of the resource reference for accessing the picture
        return GetTable()[iInstrument].strResourceReference;
    }
    else
    {
        return "";
    }
}

QString CInstPictures::GetName ( const int iInstrument )
{
    // range check
    if ( IsInstIndexInRange ( iInstrument ) )
    {
        // return the name of the instrument
        return GetTable()[iInstrument].strName;
    }
    else
    {
        return "";
    }
}

CInstPictures::EInstCategory CInstPictures::GetCategory ( const int iInstrument )
{
    // range check
    if ( IsInstIndexInRange ( iInstrument ) )
    {
        // return the name of the instrument
        return GetTable()[iInstrument].eInstCategory;
    }
    else
    {
        return IC_OTHER_INSTRUMENT;
    }
}

// Locale management class -----------------------------------------------------
QString CLocale::GetCountryFlagIconsResourceReference ( const QLocale::Country eCountry )
{
    QString strReturn = "";

    // special flag for none
    if ( eCountry == QLocale::AnyCountry )
    {
        strReturn = ":/png/flags/res/flags/flagnone.png";
    }
    else
    {
        // NOTE: The following code was introduced to support old QT versions. The problem
        //       is that the number of countries displayed is less than the one displayed
        //       with the new code below (which is disabled). Therefore, as soon as the
        //       compatibility to the very old versions of QT is not required anymore, use
        //       the new code.
        // COMPATIBLE FOR OLD QT VERSIONS -> use a table:
        QString strISO3166 = "";
        switch ( static_cast<int> ( eCountry ) )
        {
        case 1:
            strISO3166 = "af";
            break;
        case 2:
            strISO3166 = "al";
            break;
        case 3:
            strISO3166 = "dz";
            break;
        case 5:
            strISO3166 = "ad";
            break;
        case 6:
            strISO3166 = "ao";
            break;
        case 10:
            strISO3166 = "ar";
            break;
        case 11:
            strISO3166 = "am";
            break;
        case 12:
            strISO3166 = "aw";
            break;
        case 13:
            strISO3166 = "au";
            break;
        case 14:
            strISO3166 = "at";
            break;
        case 15:
            strISO3166 = "az";
            break;
        case 17:
            strISO3166 = "bh";
            break;
        case 18:
            strISO3166 = "bd";
            break;
        case 20:
            strISO3166 = "by";
            break;
        case 21:
            strISO3166 = "be";
            break;
        case 23:
            strISO3166 = "bj";
            break;
        case 25:
            strISO3166 = "bt";
            break;
        case 26:
            strISO3166 = "bo";
            break;
        case 27:
            strISO3166 = "ba";
            break;
        case 28:
            strISO3166 = "bw";
            break;
        case 30:
            strISO3166 = "br";
            break;
        case 32:
            strISO3166 = "bn";
            break;
        case 33:
            strISO3166 = "bg";
            break;
        case 34:
            strISO3166 = "bf";
            break;
        case 35:
            strISO3166 = "bi";
            break;
        case 36:
            strISO3166 = "kh";
            break;
        case 37:
            strISO3166 = "cm";
            break;
        case 38:
            strISO3166 = "ca";
            break;
        case 39:
            strISO3166 = "cv";
            break;
        case 41:
            strISO3166 = "cf";
            break;
        case 42:
            strISO3166 = "td";
            break;
        case 43:
            strISO3166 = "cl";
            break;
        case 44:
            strISO3166 = "cn";
            break;
        case 47:
            strISO3166 = "co";
            break;
        case 48:
            strISO3166 = "km";
            break;
        case 49:
            strISO3166 = "cd";
            break;
        case 50:
            strISO3166 = "cg";
            break;
        case 52:
            strISO3166 = "cr";
            break;
        case 53:
            strISO3166 = "ci";
            break;
        case 54:
            strISO3166 = "hr";
            break;
        case 55:
            strISO3166 = "cu";
            break;
        case 56:
            strISO3166 = "cy";
            break;
        case 57:
            strISO3166 = "cz";
            break;
        case 58:
            strISO3166 = "dk";
            break;
        case 59:
            strISO3166 = "dj";
            break;
        case 61:
            strISO3166 = "do";
            break;
        case 62:
            strISO3166 = "tl";
            break;
        case 63:
            strISO3166 = "ec";
            break;
        case 64:
            strISO3166 = "eg";
            break;
        case 65:
            strISO3166 = "sv";
            break;
        case 66:
            strISO3166 = "gq";
            break;
        case 67:
            strISO3166 = "er";
            break;
        case 68:
            strISO3166 = "ee";
            break;
        case 69:
            strISO3166 = "et";
            break;
        case 71:
            strISO3166 = "fo";
            break;
        case 73:
            strISO3166 = "fi";
            break;
        case 74:
            strISO3166 = "fr";
            break;
        case 76:
            strISO3166 = "gf";
            break;
        case 77:
            strISO3166 = "pf";
            break;
        case 79:
            strISO3166 = "ga";
            break;
        case 81:
            strISO3166 = "ge";
            break;
        case 82:
            strISO3166 = "de";
            break;
        case 83:
            strISO3166 = "gh";
            break;
        case 85:
            strISO3166 = "gr";
            break;
        case 86:
            strISO3166 = "gl";
            break;
        case 88:
            strISO3166 = "gp";
            break;
        case 90:
            strISO3166 = "gt";
            break;
        case 91:
            strISO3166 = "gn";
            break;
        case 92:
            strISO3166 = "gw";
            break;
        case 93:
            strISO3166 = "gy";
            break;
        case 96:
            strISO3166 = "hn";
            break;
        case 97:
            strISO3166 = "hk";
            break;
        case 98:
            strISO3166 = "hu";
            break;
        case 99:
            strISO3166 = "is";
            break;
        case 100:
            strISO3166 = "in";
            break;
        case 101:
            strISO3166 = "id";
            break;
        case 102:
            strISO3166 = "ir";
            break;
        case 103:
            strISO3166 = "iq";
            break;
        case 104:
            strISO3166 = "ie";
            break;
        case 105:
            strISO3166 = "il";
            break;
        case 106:
            strISO3166 = "it";
            break;
        case 108:
            strISO3166 = "jp";
            break;
        case 109:
            strISO3166 = "jo";
            break;
        case 110:
            strISO3166 = "kz";
            break;
        case 111:
            strISO3166 = "ke";
            break;
        case 113:
            strISO3166 = "kp";
            break;
        case 114:
            strISO3166 = "kr";
            break;
        case 115:
            strISO3166 = "kw";
            break;
        case 116:
            strISO3166 = "kg";
            break;
        case 117:
            strISO3166 = "la";
            break;
        case 118:
            strISO3166 = "lv";
            break;
        case 119:
            strISO3166 = "lb";
            break;
        case 120:
            strISO3166 = "ls";
            break;
        case 122:
            strISO3166 = "ly";
            break;
        case 123:
            strISO3166 = "li";
            break;
        case 124:
            strISO3166 = "lt";
            break;
        case 125:
            strISO3166 = "lu";
            break;
        case 126:
            strISO3166 = "mo";
            break;
        case 127:
            strISO3166 = "mk";
            break;
        case 128:
            strISO3166 = "mg";
            break;
        case 130:
            strISO3166 = "my";
            break;
        case 132:
            strISO3166 = "ml";
            break;
        case 133:
            strISO3166 = "mt";
            break;
        case 135:
            strISO3166 = "mq";
            break;
        case 136:
            strISO3166 = "mr";
            break;
        case 137:
            strISO3166 = "mu";
            break;
        case 138:
            strISO3166 = "yt";
            break;
        case 139:
            strISO3166 = "mx";
            break;
        case 141:
            strISO3166 = "md";
            break;
        case 142:
            strISO3166 = "mc";
            break;
        case 143:
            strISO3166 = "mn";
            break;
        case 145:
            strISO3166 = "ma";
            break;
        case 146:
            strISO3166 = "mz";
            break;
        case 147:
            strISO3166 = "mm";
            break;
        case 148:
            strISO3166 = "na";
            break;
        case 150:
            strISO3166 = "np";
            break;
        case 151:
            strISO3166 = "nl";
            break;
        case 153:
            strISO3166 = "nc";
            break;
        case 154:
            strISO3166 = "nz";
            break;
        case 155:
            strISO3166 = "ni";
            break;
        case 156:
            strISO3166 = "ne";
            break;
        case 157:
            strISO3166 = "ng";
            break;
        case 161:
            strISO3166 = "no";
            break;
        case 162:
            strISO3166 = "om";
            break;
        case 163:
            strISO3166 = "pk";
            break;
        case 165:
            strISO3166 = "ps";
            break;
        case 166:
            strISO3166 = "pa";
            break;
        case 167:
            strISO3166 = "pg";
            break;
        case 168:
            strISO3166 = "py";
            break;
        case 169:
            strISO3166 = "pe";
            break;
        case 170:
            strISO3166 = "ph";
            break;
        case 172:
            strISO3166 = "pl";
            break;
        case 173:
            strISO3166 = "pt";
            break;
        case 174:
            strISO3166 = "pr";
            break;
        case 175:
            strISO3166 = "qa";
            break;
        case 176:
            strISO3166 = "re";
            break;
        case 177:
            strISO3166 = "ro";
            break;
        case 178:
            strISO3166 = "ru";
            break;
        case 179:
            strISO3166 = "rw";
            break;
        case 184:
            strISO3166 = "sm";
            break;
        case 185:
            strISO3166 = "st";
            break;
        case 186:
            strISO3166 = "sa";
            break;
        case 187:
            strISO3166 = "sn";
            break;
        case 188:
            strISO3166 = "sc";
            break;
        case 189:
            strISO3166 = "sl";
            break;
        case 190:
            strISO3166 = "sg";
            break;
        case 191:
            strISO3166 = "sk";
            break;
        case 192:
            strISO3166 = "si";
            break;
        case 194:
            strISO3166 = "so";
            break;
        case 195:
            strISO3166 = "za";
            break;
        case 197:
            strISO3166 = "es";
            break;
        case 198:
            strISO3166 = "lk";
            break;
        case 201:
            strISO3166 = "sd";
            break;
        case 202:
            strISO3166 = "sr";
            break;
        case 204:
            strISO3166 = "sz";
            break;
        case 205:
            strISO3166 = "se";
            break;
        case 206:
            strISO3166 = "ch";
            break;
        case 207:
            strISO3166 = "sy";
            break;
        case 208:
            strISO3166 = "tw";
            break;
        case 209:
            strISO3166 = "tj";
            break;
        case 210:
            strISO3166 = "tz";
            break;
        case 211:
            strISO3166 = "th";
            break;
        case 212:
            strISO3166 = "tg";
            break;
        case 214:
            strISO3166 = "to";
            break;
        case 216:
            strISO3166 = "tn";
            break;
        case 217:
            strISO3166 = "tr";
            break;
        case 221:
            strISO3166 = "ug";
            break;
        case 222:
            strISO3166 = "ua";
            break;
        case 223:
            strISO3166 = "ae";
            break;
        case 224:
            strISO3166 = "gb";
            break;
        case 225:
            strISO3166 = "us";
            break;
        case 227:
            strISO3166 = "uy";
            break;
        case 228:
            strISO3166 = "uz";
            break;
        case 231:
            strISO3166 = "ve";
            break;
        case 232:
            strISO3166 = "vn";
            break;
        case 236:
            strISO3166 = "eh";
            break;
        case 237:
            strISO3166 = "ye";
            break;
        case 239:
            strISO3166 = "zm";
            break;
        case 240:
            strISO3166 = "zw";
            break;
        case 242:
            strISO3166 = "me";
            break;
        case 243:
            strISO3166 = "rs";
            break;
        case 248:
            strISO3166 = "ax";
            break;
        }
        strReturn = ":/png/flags/res/flags/" + strISO3166 + ".png";

        // check if file actually exists, if not then invalidate reference
        if ( !QFile::exists ( strReturn ) )
        {
            strReturn = "";
        }

        // AT LEAST QT 4.8 IS REQUIRED:
        /*
                // There is no direct query of the country code in Qt, therefore we use a
                // workaround: Get the matching locales properties and split the name of
                // that since the second part is the country code
                QList<QLocale> vCurLocaleList = QLocale::matchingLocales ( QLocale::AnyLanguage,
                                                                           QLocale::AnyScript,
                                                                           eCountry );

                // check if the matching locales query was successful
                if ( vCurLocaleList.size() > 0 )
                {
                    QStringList vstrLocParts = vCurLocaleList.at ( 0 ).name().split("_");

                    // the second split contains the name we need
                    if ( vstrLocParts.size() > 1 )
                    {
                        strReturn = ":/png/flags/res/flags/" + vstrLocParts.at ( 1 ).toLower() + ".png";

                        // check if file actually exists, if not then invalidate reference
                        if ( !QFile::exists ( strReturn ) )
                        {
                            strReturn = "";
                        }
        //else
        //{
        //// TEST generate table
        //static FILE* pFile = fopen ( "test.dat", "w" );
        //fprintf ( pFile, "            case %d: strISO3166 = \"%s\"; break;\n",
        //          static_cast<int> ( eCountry ), vstrLocParts.at ( 1 ).toLower().toStdString().c_str() );
        //fflush ( pFile );
        //}
                    }
                }
        */
    }

    return strReturn;
}

QMap<QString, QString> CLocale::GetAvailableTranslations()
{
    QMap<QString, QString> TranslMap;
    QDirIterator           DirIter ( ":/translations" );

    // add english language (default which is in the actual source code)
    TranslMap["en"] = ""; // empty file name means that the translation load fails and we get the default english language

    while ( DirIter.hasNext() )
    {
        // get alias of translation file
        const QString strCurFileName = DirIter.next();

        // extract only language code (must be at the end, separated with a "_")
        const QString strLoc = strCurFileName.right ( strCurFileName.length() - strCurFileName.indexOf ( "_" ) - 1 );

        TranslMap[strLoc] = strCurFileName;
    }

    return TranslMap;
}

QPair<QString, QString> CLocale::FindSysLangTransFileName ( const QMap<QString, QString>& TranslMap )
{
    QPair<QString, QString> PairSysLang ( "", "" );
    QStringList             slUiLang = QLocale().uiLanguages();

    if ( !slUiLang.isEmpty() )
    {
        QString strUiLang = QLocale().uiLanguages().at ( 0 );
        strUiLang.replace ( "-", "_" );

        // first try to find the complete language string
        if ( TranslMap.constFind ( strUiLang ) != TranslMap.constEnd() )
        {
            PairSysLang.first  = strUiLang;
            PairSysLang.second = TranslMap[PairSysLang.first];
        }
        else
        {
            // only extract two first characters to identify language (ignoring
            // location for getting a simpler implementation -> if the language
            // is not correct, the user can change it in the GUI anyway)
            if ( strUiLang.length() >= 2 )
            {
                PairSysLang.first  = strUiLang.left ( 2 );
                PairSysLang.second = TranslMap[PairSysLang.first];
            }
        }
    }

    return PairSysLang;
}

void CLocale::LoadTranslation ( const QString strLanguage, QCoreApplication* pApp )
{
    // The translator objects must be static!
    static QTranslator myappTranslator;
    static QTranslator myqtTranslator;

    QMap<QString, QString> TranslMap              = CLocale::GetAvailableTranslations();
    const QString          strTranslationFileName = TranslMap[strLanguage];

    if ( myappTranslator.load ( strTranslationFileName ) )
    {
        pApp->installTranslator ( &myappTranslator );
    }

    // allows the Qt messages to be translated in the application
    if ( myqtTranslator.load ( QLocale ( strLanguage ), "qt", "_", QLibraryInfo::location ( QLibraryInfo::TranslationsPath ) ) )
    {
        pApp->installTranslator ( &myqtTranslator );
    }
}

/******************************************************************************\
* Global Functions Implementation                                              *
\******************************************************************************/
QString GetVersionAndNameStr ( const bool bWithHtml )
{
    QString strVersionText = "";

    // name, short description and GPL hint
    if ( bWithHtml )
    {
        strVersionText += "<b>";
    }
    else
    {
        strVersionText += " *** ";
    }

    strVersionText += APP_NAME + QCoreApplication::tr ( ", Version " ) + VERSION;

    if ( bWithHtml )
    {
        strVersionText += "</b><br>";
    }
    else
    {
        strVersionText += "\n *** ";
    }

    if ( !bWithHtml )
    {
        strVersionText += QCoreApplication::tr ( "Internet Jam Session Software" );
        strVersionText += "\n *** ";
    }

    strVersionText += QCoreApplication::tr ( "Released under the GNU General Public License (GPL)" );

    return strVersionText;
}

QString MakeClientNameTitle ( QString win, QString client )
{
    QString sReturnString = win;
    if ( !client.isEmpty() )
    {
        sReturnString += " - " + client;
    }
    return ( sReturnString );
}
