import java.io.DataInput;
import java.io.IOException;

public class ChdescSetOwner extends Opcode
{
	private final int chdesc, owner;
	
	public ChdescSetOwner(int chdesc, int owner)
	{
		this.chdesc = chdesc;
		this.owner = owner;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
			chdesc.setOwner(owner);
	}
	
	public String toString()
	{
		return "KDB_CHDESC_SET_OWNER: chdesc = " + SystemState.hex(chdesc) + ", owner = " + SystemState.hex(owner);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_SET_OWNER, "KDB_CHDESC_SET_OWNER", ChdescSetOwner.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("owner", 4);
		return factory;
	}
}
