import java.util.HashMap;

public class SystemState
{
	private HashMap chdescs;
	
	public SystemState()
	{
		chdescs = new HashMap();
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
