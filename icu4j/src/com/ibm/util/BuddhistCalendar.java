/*
 *******************************************************************************
 * Copyright (C) 1996-2000, International Business Machines Corporation and    *
 * others. All Rights Reserved.                                                *
 *******************************************************************************
 *
 * $Source: /xsrl/Nsvn/icu/icu4j/src/com/ibm/util/Attic/BuddhistCalendar.java,v $ 
 * $Date: 2000/11/18 00:30:54 $ 
 * $Revision: 1.6 $
 *
 *****************************************************************************************
 */

package com.ibm.util;

import java.util.Date;
import com.ibm.util.GregorianCalendar;
import java.util.Locale;

/**
 * <code>BuddhistCalendar</code> is a subclass of <code>GregorianCalendar</code>
 * that numbers years since the birth of the Buddha.  This is the civil calendar
 * in some predominantly Buddhist countries such as Thailand, and it is used for
 * religious purposes elsewhere.
 * <p>
 * The Buddhist calendar is identical to the Gregorian calendar in all respects
 * except for the year and era.  Years are numbered since the birth of the
 * Buddha in 543 BC (Gregorian), so that 1 AD (Gregorian) is equivalent to 544
 * BE (Buddhist Era) and 1998 AD is 2541 BE.
 * <p>
 * The Buddhist Calendar has only one allowable era: <code>BE</code>.  If the
 * calendar is not in lenient mode (see <code>setLenient</code>), dates before
 * 1/1/1 BE are rejected with an <code>IllegalArgumentException</code>.
 *
 * @see com.ibm.util.GregorianCalendar
 *
 * @author Laura Werner
 * @author Alan Liu
 */
public class BuddhistCalendar extends GregorianCalendar {
    
    private static String copyright = "Copyright \u00a9 1998 IBM Corp. All Rights Reserved.";

    //-------------------------------------------------------------------------
    // Constructors...
    //-------------------------------------------------------------------------

    /**
     * Constant for the Buddhist Era.  This is the only allowable <code>ERA</code>
     * value for the Buddhist calendar.
     *
     * @see com.ibm.util.Calendar#ERA
     */
    public static final int BE = 0;
    
    /**
     * Constructs a <code>BuddhistCalendar</code> using the current time
     * in the default time zone with the default locale.
     */
    public BuddhistCalendar() {
        super();
    }

    /**
     * Constructs a <code>BuddhistCalendar</code> based on the current time
     * in the given time zone with the default locale.
     *
     * @param zone the given time zone.
     */
    public BuddhistCalendar(TimeZone zone) {
        super(zone);
    }

    /**
     * Constructs a <code>BuddhistCalendar</code> based on the current time
     * in the default time zone with the given locale.
     *
     * @param aLocale the given locale.
     */
    public BuddhistCalendar(Locale aLocale) {
        super(aLocale);
    }

    /**
     * Constructs a <code>BuddhistCalendar</code> based on the current time
     * in the given time zone with the given locale.
     *
     * @param zone the given time zone.
     *
     * @param aLocale the given locale.
     */
    public BuddhistCalendar(TimeZone zone, Locale aLocale) {
        super(zone, aLocale);
    }

    /**
     * Constructs a <code>BuddhistCalendar</code> with the given date set
     * in the default time zone with the default locale.
     *
     * @param date      The date to which the new calendar is set.
     */
    public BuddhistCalendar(Date date) {
        this();
        setTime(date);
    }

    /**
     * Constructs a <code>BuddhistCalendar</code> with the given date set
     * in the default time zone with the default locale.
     *
     * @param year      The value used to set the calendar's {@link #YEAR YEAR} time field.
     *
     * @param month     The value used to set the calendar's {@link #MONTH MONTH} time field.
     *                  The value is 0-based. e.g., 0 for January.
     *
     * @param date      The value used to set the calendar's {@link #DATE DATE} time field.
     */
    public BuddhistCalendar(int year, int month, int date) {
        super(year, month, date);
    }

    /**
     * Constructs a BuddhistCalendar with the given date
     * and time set for the default time zone with the default locale.
     *
     * @param year      The value used to set the calendar's {@link #YEAR YEAR} time field.
     *
     * @param month     The value used to set the calendar's {@link #MONTH MONTH} time field.
     *                  The value is 0-based. e.g., 0 for January.
     *
     * @param date      The value used to set the calendar's {@link #DATE DATE} time field.
     *
     * @param hour      The value used to set the calendar's {@link #HOUR_OF_DAY HOUR_OF_DAY} time field.
     *
     * @param minute    The value used to set the calendar's {@link #MINUTE MINUTE} time field.
     *
     * @param second    The value used to set the calendar's {@link #SECOND SECOND} time field.
     */
    public BuddhistCalendar(int year, int month, int date, int hour,
                             int minute, int second)
    {
        super(year, month, date, hour, minute, second);
    }


    //-------------------------------------------------------------------------
    // The only practical difference from a Gregorian calendar is that years
    // are numbered since the birth of the Buddha.  A couple of overrides will
    // take care of that....
    //-------------------------------------------------------------------------
    
    // Starts in -543 AD, ie 544 BC
    private static final int BUDDHIST_ERA_START = -543;
    
    protected int handleGetExtendedYear() {
        int year;
        if (newerField(EXTENDED_YEAR, YEAR) == EXTENDED_YEAR) {
            year = internalGet(EXTENDED_YEAR, 1);
        } else {
            // Ignore the era, as there is only one
            year = internalGet(YEAR, 1);
        }
        return year;
    }

    // Return JD of start of given month/year
    protected int handleComputeMonthStart(int eyear, int month) {
        return super.handleComputeMonthStart(eyear + BUDDHIST_ERA_START, month);
    }

    protected void handleComputeFields(int julianDay) {
        super.handleComputeFields(julianDay);
        int y = internalGet(EXTENDED_YEAR) - BUDDHIST_ERA_START;
        internalSet(EXTENDED_YEAR, y);
        internalSet(ERA, 0);
        internalSet(YEAR, y);
    }

    /**
     * Override GregorianCalendar.  There is only one Buddhist ERA.  We
     * should really handle YEAR, YEAR_WOY, and EXTENDED_YEAR here too to
     * implement the 1..5000000 range, but it's not critical.
     */
    protected int handleGetLimit(int field, int limitType) {
        if (field == ERA) {
            return BE;
        }
        return super.handleGetLimit(field, limitType);
    }
}
