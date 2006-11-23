import java.util.*;
import java.io.*;

import command.*;

public class SATWritesSolver
{
	public static void makeSATInstance(SystemState state, int k, PrintStream output)
	{
		int n = state.getChdescCount();
		Map map = new HashMap();
		int i = 0, total = 0;
		
		/* build a map from chdesc to index */
		Iterator chdescs = state.getChdescs();
		while(chdescs.hasNext())
			map.put(chdescs.next(), new Integer(i++));
		
		/* we have a table of literals, with n rows and k columns */
		/* the literals are indexed like pixels in a linear frame buffer */
		
		/* first calculate the total number of clauses */
		chdescs = state.getChdescs();
		while(chdescs.hasNext())
		{
			Chdesc chdesc = (Chdesc) chdescs.next();
			int chdescIndex = ((Integer) map.get(chdesc)).intValue();
			
			total += k * (k - 1) / 2 * chdesc.getAfterCount();
			
			if(chdesc.getType() == Chdesc.TYPE_NOOP)
				continue;
			Iterator all = state.getChdescs();
			while(all.hasNext())
			{
				Chdesc other = (Chdesc) all.next();
				if(other.getType() == Chdesc.TYPE_NOOP)
					continue;
				if(chdesc.getBlock() == other.getBlock())
					continue;
				int otherIndex = ((Integer) map.get(other)).intValue();
				/* we only need one direction */
				if(otherIndex < chdescIndex)
					continue;
				total += k;
			}
		}
		total += n;
		
		/* output the header */
		output.println("c chdescs = " + n + ", writes <= " + k);
		output.println("p cnf " + (n * k) + " " + total);

		chdescs = state.getChdescs();
		while(chdescs.hasNext())
		{
			Chdesc chdesc = (Chdesc) chdescs.next();
			int chdescIndex = ((Integer) map.get(chdesc)).intValue();
			Iterator afters = chdesc.getAfters();
			while(afters.hasNext())
			{
				Chdesc after = (Chdesc) afters.next();
				int afterIndex = ((Integer) map.get(after)).intValue();
				int rowOffset = afterIndex * k + 1;
				for(i = 1; i < k; i++)
					/* each after must not occur before the current i */
					for(int j = 0; j < i; j++)
						output.println("-" + (rowOffset + j) + " -" + (chdescIndex * k + i + 1) + " 0");
			}
			if(chdesc.getType() == Chdesc.TYPE_NOOP)
				continue;
			for(i = 0; i < k; i++)
			{
				int chdescNumber = chdescIndex * k + i + 1;
				Iterator all = state.getChdescs();
				while(all.hasNext())
				{
					Chdesc other = (Chdesc) all.next();
					if(other.getType() == Chdesc.TYPE_NOOP)
						continue;
					if(chdesc.getBlock() == other.getBlock())
						continue;
					int otherIndex = ((Integer) map.get(other)).intValue();
					/* we only need one direction */
					if(otherIndex < chdescIndex)
						continue;
					output.println("-" + chdescNumber + " -" + (otherIndex * k + i + 1) + " 0");
				}
			}
		}

		/* generate the row clauses: each chdesc is written at least once */
		for(i = 0; i < n; i++)
		{
			StringBuffer clause = new StringBuffer();
			for(int j = 0; j < k; j++)
				clause.append((i * k + j + 1) + " ");
			clause.append("0");
			output.println(clause);
		}
	}
	
	public static int findMinBlockWrites(SystemState state)
	{
		int min = 0, max = state.getChdescCount();
		boolean satisfiable = false;
		/* binary search the minimum k from 0-n */
		while(min < max)
		{
			/* avoid overflow */
			int k = min + ((max - min) / 2);
			int value;
			Process p;

			System.out.println("Trying k = " + k);
			try {
				byte junk[] = new byte[4096];
				p = Runtime.getRuntime().exec("minisat");

				BufferedOutputStream out = new BufferedOutputStream(p.getOutputStream());
				makeSATInstance(state, k, new PrintStream(out));
				out.flush();
				p.getOutputStream().close();
				while(p.getInputStream().read(junk) > 0);
			}
			catch(IOException e)
			{
				throw new RuntimeException(e);
			}

			/* keep trying until we aren't interrupted */
			for(;;)
				try {
					value = p.waitFor();
					break;
				}
			catch(InterruptedException e)
			{
			}
			if(value == 10)
			{
				/* satisfiable */
				max = k;
				satisfiable = true;
			}
			else if(value == 20)
				/* unsatisfiable */
				min = k + 1;
			else
				throw new RuntimeException("What the hell? (value = " + value + ")");
		}
		if(!satisfiable)
			throw new RuntimeException("What the hell? (not satisfiable)");
		return min;
	}
}
