import java.util.HashMap;
import java.util.Iterator;
import java.io.Writer;
import java.io.IOException;

public class SystemState
{
	private HashMap bdescs;
	public final ChdescCollection chdescs;
	
	public SystemState()
	{
		bdescs = new HashMap();
		chdescs = new ChdescCollection("registered");
	}
	
	public void addBdesc(Bdesc bdesc)
	{
		//Integer key = Integer.valueOf(bdesc.address);
		Integer key = new Integer(bdesc.address);
		if(bdescs.containsKey(key))
			throw new RuntimeException("Duplicate bdesc registered!");
		//System.out.println("Add " + bdesc);
		bdescs.put(key, bdesc);
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
		//System.out.println("Destroy " + lookupBdesc(bdesc));
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
	
	public void render(Writer output) throws IOException
	{
		output.write("digraph chdescs\n{\nsize=\"11,8.5\";\nnodesep=0.25;\nranksep=0.25;\norientation=L;\nnode [shape=circle,color=black];\n");
		Iterator i = chdescs.iterator();
		while(i.hasNext())
		{
			Chdesc chdesc = (Chdesc) i.next();
			output.write(chdesc.render());
		}
		output.write("}\n");
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
