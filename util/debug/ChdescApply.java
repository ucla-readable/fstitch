import java.io.DataInput;
import java.io.IOException;

public class ChdescApply extends Opcode
{
	private final int chdesc;
	
	public ChdescApply(int chdesc)
	{
		this.chdesc = chdesc;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
			chdesc.clearFlags(Chdesc.FLAG_ROLLBACK);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_APPLY, "KDB_CHDESC_APPLY", ChdescApply.class);
		factory.addParameter("chdesc", 4);
		return factory;
	}
}
