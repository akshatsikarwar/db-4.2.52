/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2002-2003
 *	Sleepycat Software.  All rights reserved.
 *
 * $Id: PartKey.java,v 1.8 2003/10/18 19:50:20 mhayes Exp $
 */

package com.sleepycat.examples.bdb.shipment.entity;

import java.io.Serializable;

/**
 * A PartKey serves as the key in the key/value pair for a part entity.
 *
 * <p> In this sample, PartKey is used both as the storage data for the key as
 * well as the object binding to the key.  Because it is used directly as
 * storage data using SerialFormat, it must be Serializable. </p>
 *
 * @author Mark Hayes
 */
public class PartKey implements Serializable {

    private String number;

    public PartKey(String number) {

        this.number = number;
    }

    public final String getNumber() {

        return number;
    }

    public String toString() {

        return "[PartKey: number=" + number + ']';
    }
}
