/////////////////////////////////////////////////////////////////////////////
//
// Backport from wxWidgets-3.0-rc1
//
/////////////////////////////////////////////////////////////////////////////
// Name:        src/common/valnum.cpp
// Purpose:     Numeric validator classes implementation
// Author:      Vadim Zeitlin based on the submission of Fulvio Senore
// Created:     2010-11-06
// Copyright:   (c) 2010 wxWidgets team
// Licence:     wxWindows licence
/////////////////////////////////////////////////////////////////////////////

// ============================================================================
// Declarations
// ============================================================================

// ----------------------------------------------------------------------------
// headers
// ----------------------------------------------------------------------------

// For compilers that support precompilation, includes "wx.h".
#include <wx/wxprec.h>

#ifdef __BORLANDC__
    #pragma hdrstop
#endif

#if wxUSE_VALIDATORS && wxUSE_TEXTCTRL

#ifndef WX_PRECOMP
    #include <wx/msgdlg.h>
    #include <wx/textctrl.h>
#endif

#include "valnum.h"
#include "numformatter.h"

// ============================================================================
// NumValidatorBase implementation
// ============================================================================

BEGIN_EVENT_TABLE(NumValidatorBase, wxValidator)
    EVT_CHAR(NumValidatorBase::OnChar)
    EVT_KILL_FOCUS(NumValidatorBase::OnKillFocus)
END_EVENT_TABLE()

int NumValidatorBase::GetFormatFlags() const
{
    int flags = NumberFormatter::Style_None;
    if ( m_style & NUM_VAL_THOUSANDS_SEPARATOR )
        flags |= NumberFormatter::Style_WithThousandsSep;
    if ( m_style & NUM_VAL_NO_TRAILING_ZEROES )
        flags |= NumberFormatter::Style_NoTrailingZeroes;
    if ( m_style & NUM_VAL_ONE_TRAILING_ZERO )
        flags |= NumberFormatter::Style_OneTrailingZero;
    if ( m_style & NUM_VAL_TWO_TRAILING_ZEROES )
        flags |= NumberFormatter::Style_TwoTrailingZeroes;
    if ( m_style & NUM_VAL_THREE_TRAILING_ZEROES )
        flags |= NumberFormatter::Style_ThreeTrailingZeroes;

    return flags;
}

wxTextEntry *NumValidatorBase::GetTextEntry() const
{
#if wxUSE_TEXTCTRL
    if ( wxTextCtrl *text = wxDynamicCast(m_validatorWindow, wxTextCtrl) )
        return text;
#endif // wxUSE_TEXTCTRL

    wxFAIL_MSG(wxT("Can only be used with wxTextCtrl or wxComboBox"));

    return NULL;
}

bool NumValidatorBase::Validate(wxWindow *parent)
{
    // If window is disabled, simply return
    if ( !m_validatorWindow->IsEnabled() )
        return true;

    wxString errmsg;
    bool res = DoValidateNumber(&errmsg);

    if ( !res )
    {
        wxMessageBox(errmsg, _("Validation error"),
                     wxOK | wxICON_ERROR, parent);
        m_validatorWindow->SetFocus();
        return false;
    }

    return true;
}

void
NumValidatorBase::GetCurrentValueAndInsertionPoint(wxString& val,
                                                             int& pos) const
{
    wxTextEntry * const control = GetTextEntry();
    if ( !control )
        return;

    val = control->GetValue();
    pos = control->GetInsertionPoint();

    long selFrom, selTo;
    control->GetSelection(&selFrom, &selTo);

    const long selLen = selTo - selFrom;
    if ( selLen )
    {
        // Remove selected text because pressing a key would make it disappear.
        val.erase(selFrom, selLen);

        // And adjust the insertion point to have correct position in the new
        // string.
        if ( pos > selFrom )
        {
            if ( pos >= selTo )
                pos -= selLen;
            else
                pos = selFrom;
        }
    }
}

bool NumValidatorBase::IsMinusOk(const wxString& val, int pos) const
{
    // Minus is only ever accepted in the beginning of the string.
    if ( pos != 0 )
        return false;

    // And then only if there is no existing minus sign there.
    if ( !val.empty() && val[0] == '-' )
        return false;

    return true;
}

void NumValidatorBase::OnChar(wxKeyEvent& event)
{
    // By default we just validate this key so don't prevent the normal
    // handling from taking place.
    event.Skip();

    if ( !m_validatorWindow )
        return;

#if wxUSE_UNICODE
    const int ch = event.GetUnicodeKey();
    const int c = event.GetKeyCode();
    if ( c > WXK_START )
    {
        // It's a character without any Unicode equivalent at all, e.g. cursor
        // arrow or function key, we never filter those.
        return;
    }
#else // !wxUSE_UNICODE
    const int ch = event.GetKeyCode();
    const int c = ch;
    if ( ch > WXK_DELETE )
    {
        // Not a character neither.
        return;
    }
#endif // wxUSE_UNICODE/!wxUSE_UNICODE

    // Space is an allowed thousands separator. But we don't allow user to type
    // it. We will add it at formatting time in OnKillFocus().
    if ( c < WXK_SPACE || c == WXK_DELETE )
    {
        // Allow ASCII control characters and Delete.
        return;
    }

    // Check if this character is allowed in the current state.
    wxString val;
    int pos;
    GetCurrentValueAndInsertionPoint(val, pos);

    if ( !IsCharOk(val, pos, ch) )
    {
        if ( !wxValidator::IsSilent() )
            wxBell();

        // Do not skip the event in this case, stop handling it here.
        event.Skip(false);
    }
}

void NumValidatorBase::OnKillFocus(wxFocusEvent& event)
{
    wxTextEntry * const control = GetTextEntry();
    if ( !control )
        return;

    // When we change the control value below, its "modified" status is reset
    // so we need to explicitly keep it marked as modified if it was so in the
    // first place.
    //
    // Notice that only wxTextCtrl (and not wxTextEntry) has
    // IsModified()/MarkDirty() methods hence the need for dynamic cast.
    wxTextCtrl * const text = wxDynamicCast(m_validatorWindow, wxTextCtrl);
    const bool wasModified = text ? text->IsModified() : false;

    control->ChangeValue(NormalizeString(control->GetValue()));

    if ( wasModified )
        text->MarkDirty();

    event.Skip();

//    Validate(text);
}

// ============================================================================
// IntegerValidatorBase implementation
// ============================================================================

wxString IntegerValidatorBase::ToString(LongestValueType value) const
{
    return NumberFormatter::ToString(value, GetFormatFlags());
}

bool
IntegerValidatorBase::FromString(const wxString& s, LongestValueType *value)
{
    return NumberFormatter::FromString(s, value);
}

bool
IntegerValidatorBase::IsCharOk(const wxString& val, int pos, wxChar ch) const
{
    // We may accept minus sign if we can represent negative numbers at all.
    if ( ch == '-' )
    {
        // Notice that entering '-' can make our value invalid, for example if
        // we're limited to -5..15 range and the current value is 12, then the
        // new value would be (invalid) -12. We consider it better to let the
        // user do this because perhaps he is going to press Delete key next to
        // make it -2 and forcing him to delete 1 first would be unnatural.
        //
        // TODO: It would be nice to indicate that the current control contents
        //       is invalid (if it's indeed going to be the case) once
        //       wxValidator supports doing this non-intrusively.
        return m_min < 0 && IsMinusOk(val, pos);
    }

    // A separator is accepted if the locale allow it, the other chars must be digits
    if ( ch < '0' || ch > '9' )
    {
        wxChar thousands;
        if ( NumberFormatter::GetThousandsSeparatorIfUsed(&thousands) )
        {
            if (ch != thousands)
                return false;
        }
        else
        {
            return false;
        }
    }

    return true;
}

bool IntegerValidatorBase::DoValidateNumber(wxString * errMsg) const
{
    wxTextEntry * const control = GetTextEntry();
    if ( !control )
       return false;

    wxString s(control->GetValue());
    wxChar thousandsSep;
    if ( NumberFormatter::GetThousandsSeparatorIfUsed(&thousandsSep) )
        s.Replace(wxString(thousandsSep), wxString());

    if ( s.empty() )
    {
        // Is blank, but allowed. Stop here
        if ( HasFlag(NUM_VAL_ZERO_AS_BLANK) )
        {
            return true;
        }
        // We can't do any check with an empty string
        else
        {
            *errMsg = _("Empty value");
            return false;
        }
    }

    // Can it be converted to a value?
    LongestValueType value;
    bool res = FromString(s, &value);
    if ( !res )
       *errMsg = _("Malformed number");
    else
    {
       res = IsInRange(value);
       if ( !res )
             *errMsg = _("Not in range");
    }

    return res;
}

// ============================================================================
// FloatingPointValidatorBase implementation
// ============================================================================

wxString FloatingPointValidatorBase::ToString(LongestValueType value) const
{
    return NumberFormatter::ToString(value, m_precision, GetFormatFlags());
}

bool
FloatingPointValidatorBase::FromString(const wxString& s,
                                         LongestValueType *value)
{
    return NumberFormatter::FromString(s, value);
}

bool
FloatingPointValidatorBase::IsCharOk(const wxString& val,
                                       int pos,
                                       wxChar ch) const
{
    if ( ch == '-' )
    {
        // We may accept minus sign if we can represent negative numbers at all.
        if ( pos == 0 )
            return m_min < 0 && IsMinusOk(val, pos);
        // or for the exponent definition
        else if ( val[pos-1] != 'e' && val[pos-1] != 'E' )
            return false;

        return true;
    }
    else if ( ch == '+' )
    {
        if ( pos == 0 )
            return m_max >= 0;
        else if ( val[pos-1] != 'e' && val[pos-1] != 'E' )
            return false;

        return true;
    }

    const wxChar separator = NumberFormatter::GetDecimalSeparator();
    if ( ch == separator )
    {
        if ( val.find(separator) != wxString::npos )
        {
            // There is already a decimal separator, can't insert another one.
            return false;
        }

        // Prepending a separator before the sign isn't allowed.
        if ( pos == 0 && !val.empty() && ( val[0] == '-' || val[0] == '+' ) )
            return false;

        // Otherwise always accept it, adding a decimal separator doesn't
        // change the number value and, in particular, can't make it invalid.
        // OTOH the checks below might not pass because strings like "." or
        // "-." are not valid numbers so parsing them would fail, hence we need
        // to treat it specially here.
        return true;
    }

    // Must be a digit, an exponent or a thousands separator.
    if( ( ch < '0' || ch > '9' ) && ch != 'E' && ch != 'e' )
    {
        wxChar thousands;
        if ( NumberFormatter::GetThousandsSeparatorIfUsed(&thousands) )
        {
            if (ch != thousands)
                return false;
        }
        else
        {
            return false;
        }
    }

    // Check the number of decimal digits in the final string
    wxString str(val);
    str.insert(pos, ch);
    return ValidatePrecision(str);
}

bool FloatingPointValidatorBase::DoValidateNumber(wxString * errMsg) const
{
    wxTextEntry * const control = GetTextEntry();
    if ( !control )
       return false;

    wxString s(control->GetValue());
    wxChar thousandsSep;
    if ( NumberFormatter::GetThousandsSeparatorIfUsed(&thousandsSep) )
        s.Replace(wxString(thousandsSep), wxString());

    if ( s.empty() )
    {
        if ( HasFlag(NUM_VAL_ZERO_AS_BLANK) )
            return true; //Is blank, but allowed. Stop here
        else
        {
            *errMsg = _("Empty value");
            return false; //We can't do any checks with an empty string
        }
    }

    LongestValueType value;
    bool res = FromString(s, &value); // Can it be converted to a value?
    if ( !res )
        *errMsg = _("Value overflow");
    else
    {
        res = ValidatePrecision(s);
        if ( !res )
            *errMsg = _("Too many decimal digits");
        else
        {
            res = IsInRange(value);
            if ( !res )
                *errMsg = _("Not in range");
        }
    }

    return res;
}

bool FloatingPointValidatorBase::ValidatePrecision(const wxString& s) const
{
    size_t posSep = s.find(NumberFormatter::GetDecimalSeparator());
    if ( posSep == wxString::npos )
        posSep = s.length();

    // If user typed exponent 'e' the number of decimal digits is not
    // important at all. But we must know that 'e' position.
    size_t posExp = s.Lower().Find(_("e"));
    if ( posExp == wxString::npos )
        posExp = s.length();

    // Return true if number has no more decimal digits than allowed
    return ( (int)(posExp - posSep) - 1 <= (int)m_precision );
}

#endif // wxUSE_VALIDATORS && wxUSE_TEXTCTRL
