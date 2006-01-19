import java.util.HashMap;
import java.util.Iterator;
import java.io.Writer;
import java.io.IOException;

public class SystemState
{
	private HashMap bds, bdescs;
	public final ChdescCollection chdescs;
	private Chdesc free_head;
	
	public SystemState()
	{
		bds = new HashMap();
		bdescs = new HashMap();
		chdescs = new ChdescCollection("registered");
		free_head = null;
	}
	
	public void setBdName(int bd, String name)
	{
		//Integer key = Integer.valueOf(bd);
		Integer key = new Integer(bd);
		bds.put(key, name);
	}
	
	public String getBdName(int bd)
	{
		//Integer key = Integer.valueOf(bd);
		Integer key = new Integer(bd);
		return (String) bds.get(key);
	}
	
	public void addBdesc(Bdesc bdesc)
	{
		//Integer key = Integer.valueOf(bdesc.address);
		Integer key = new Integer(bdesc.address);
		if(bdescs.containsKey(key))
			throw new RuntimeException("Duplicate bdesc registered!");
		bdescs.put(key, bdesc);
	}
	
	public void setBdesc(int bdesc, int number)
	{
		//Integer key = Integer.valueOf(bdesc);
		Integer key = new Integer(bdesc);
		bdescs.put(key, new Bdesc(bdesc, 0, number));
	}
	
	public Bdesc lookupBdesc(int bdesc)
	{
		//Integer key = Integer.valueOf(bdesc);
		Integer key = new Integer(bdesc);
		return (Bdesc) bdescs.get(key);
	}
	
	public Bdesc remBdesc(int bdesc)
	{
		//Integer key = Integer.valueOf(bdesc);
		Integer key = new Integer(bdesc);
		return (Bdesc) bdescs.remove(key);
	}
	
	public Iterator getBdescs()
	{
		return new HashMapValueIterator(bdescs);
	}
	
	public void addChdesc(Chdesc chdesc)
	{
		chdescs.add(chdesc);
	}
	
	public Chdesc lookupChdesc(int chdesc)
	{
		return chdescs.lookup(chdesc);
	}
	
	public Chdesc remChdesc(int chdesc)
	{
		return chdescs.remove(chdesc);
	}
	
	public Iterator getChdescs()
	{
		return chdescs.iterator();
	}
	
	public int getChdescCount()
	{
		return chdescs.size();
	}
	
	public void setFreeHead(Chdesc free_head)
	{
		this.free_head = free_head;
	}
	
	public void render(Writer output, String title, boolean renderFree, boolean renderBlock, boolean renderOwner, Grouper grouper, boolean landscape) throws IOException
	{
		int free = 0;
		
		output.write("digraph chdescs\n{\nnodesep=0.25;\nranksep=0.25;\n");
		if(landscape)
			output.write("rankdir=LR;\norientation=L;\nsize=\"10,7.5\";\n");
		else
			output.write("rankdir=LR;\norientation=P;\nsize=\"16,16\";\n");
		output.write("subgraph clusterAll {\nlabel=\"" + title + "\";\ncolor=white;\n");
		output.write("node [shape=ellipse,color=black];\n");
		
		Iterator i = chdescs.iterator();
		while(i.hasNext())
		{
			Chdesc chdesc = (Chdesc) i.next();

			grouper.add(chdesc);

			/* render the chdesc */
			Chdesc prev = chdesc.getFreePrev();
			boolean isFree = chdesc == free_head || prev != null;
			if(isFree)
				free++;
			if(renderFree)
				output.write(chdesc.render(true, renderBlock, renderOwner, this));
			else if(chdesc == free_head || prev == null)
				output.write(chdesc.render(!isFree, renderBlock, renderOwner, this));
		}
		
		if(free_head != null)
		{
			output.write("subgraph cluster_free {\ncolor=red;\nstyle=dashed;\n");
			if(renderFree)
			{
				output.write("label=\"Free List\";\n");
				/* try to make the free list appear in a reasonable way */
				for(Chdesc chdesc = free_head; chdesc != null; chdesc = chdesc.getFreeNext())
					output.write(chdesc.renderName() + "\n");
				if(free > 3)
				{
					double ratio = Math.sqrt(free / 1.6) / free;
					int cluster = 0;
					free = 0;
					output.write("subgraph cluster_align {\nstyle=invis;\n");
					for(Chdesc chdesc = free_head; chdesc != null; chdesc = chdesc.getFreeNext())
					{
						free++;
						if(cluster < ratio * free)
						{
							cluster++;
							output.write(chdesc.renderName() + "\n");
						}
					}
					output.write("}\n");
				}
				output.write("}\n");
			}
			else
			{
				/* just render the free list head */
				output.write("label=\"Free Head (+" + (free - 1) + ")\";\n");
		       		output.write(free_head.renderName() + "\n}\n");
			}
		}

		grouper.render("", output);
		
		/*output.write("node [shape=box,color=black];\n");
		i = bdescs.iterator();
		while(i.hasNext())
		{
			Bdesc bdesc = (Bdesc) i.next();
			output.write(bdesc.render());
		}*/
		output.write("}\n}\n");
		output.flush();
	}
	
	public static String hex(int address)
	{
		String hex = Integer.toHexString(address);
		while(hex.length() < 8)
			hex = "0" + hex;
		return "0x" + hex;
	}
}
