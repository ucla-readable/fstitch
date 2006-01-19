import java.io.IOException;
import java.io.Writer;
import java.util.Iterator;
import java.util.HashSet;
import java.util.Set;

/**
 * A Grouper that does not put its Chdescs into a subgroup.
 * Useful as the bottom layer of a hierarchy of Groupers.
 */
public class NoneGrouper implements Grouper
{
	public static class Factory implements GrouperFactory
	{
		static Factory factory = new Factory();

		private Factory()
		{
			/* singleton */
		}

		public static Factory getFactory()
		{
			return factory;
		}

		public Grouper newInstance()
		{
			return new NoneGrouper();
		}

		public String toString()
		{
			return "none";
		}
	}


	private Set chdescs; // Set<Chdesc>

	public NoneGrouper()
	{
		chdescs = new HashSet();
	}

	public void add(Chdesc c)
	{
		chdescs.add(c);
	}

	public void render(String clusterPrefix, Writer output) throws IOException
	{
		for(Iterator it = chdescs.iterator(); it.hasNext();) // for(Chdesc c : chdescs)
		{
			Chdesc c = (Chdesc) it.next();
			output.write(c.renderName() + "\n");
		}
	}
}
