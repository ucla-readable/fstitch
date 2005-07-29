import java.util.HashMap;

public class SystemState
{
	private HashMap bdescs;
	private HashMap chdescs;
	
	public SystemState()
	{
		bdescs = new HashMap();
		chdescs = new HashMap();
	}
	
	public void addBdesc(Bdesc bdesc)
	{
		//Integer key = Integer.valueOf(bdesc.address);
		Integer key = new Integer(bdesc.address);
		if(bdescs.containsKey(key))
			throw new RuntimeException("Duplicate bdesc registered!");
		bdescs.put(key, bdesc);
	}
	
	public Bdesc lookupBdesc(int bdesc)
	{
		//Integer key = Integer.valueOf(bdesc);
		Integer key = new Integer(bdesc);
		return (Bdesc) bdescs.get(key);
	}
	
	public void addChdesc(Chdesc chdesc)
	{
		//Integer key = Integer.valueOf(chdesc.address);
		Integer key = new Integer(chdesc.address);
		if(chdescs.containsKey(key))
			throw new RuntimeException("Duplicate chdesc registered!");
		chdescs.put(key, chdesc);
	}
	
	public Chdesc lookupChdesc(int chdesc)
	{
		//Integer key = Integer.valueOf(chdesc);
		Integer key = new Integer(chdesc);
		return (Chdesc) chdescs.get(key);
	}
}
