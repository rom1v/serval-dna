/**
 * Copyright (C) 2011 The Serval Project
 *
 * This file is part of Serval Software (http://www.servalproject.org)
 *
 * Serval Software is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This source code is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this source code; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

package org.servalproject.servald;

import java.util.List;
import java.util.LinkedList;

class ServalD
{
	int status;
	List<byte[]> outv;

	static
	{
		String property = System.getProperty("java.library.path");
		System.err.println("Attempting to load libservald.so from "+property);
		System.loadLibrary("servald");
	}

	private static native int rawCommand(IJniResults outv, String[] args);

	public void command(String... args)
	{
		this.outv = new LinkedList<byte[]>();
		IJniResults results = new JniResultsList(outv);
		this.status = this.rawCommand(results, args);
	}

	public static void main(String[] args)
	{
		ServalD servald = new ServalD();
		servald.command(args);
		for (byte[] a: servald.outv) {
			System.out.println(new String(a));
		}
		System.exit(servald.status);
	}
}
